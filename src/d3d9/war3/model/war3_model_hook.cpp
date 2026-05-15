// war3_model_hook.cpp - War3 runtime-model / pose 被动探针

#include "war3_model_hook.h"

#include "war3_model_resource_cache.h"
#include "war3_model_registry.h"
#include "war3_direct_pose_cache.h"

#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_hook.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_runtime_profile.h"
#include "../game/war3_agent.h"
#include "../game/war3_unit.h"
#include "../hooks/war3_hook_install_util.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_render_objects.h"
#include "../render/war3_render_state.h"
#include "../render/war3_current_draw_contract.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../render/war3_visible_renderables.h"
#include "../state/war3_render_state.h"
#include "../../util/util_env.h"

#include <emmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dxvk {
namespace war3 {
namespace model {

namespace {
using CreateSpriteAndBindSourceObjectFn =
    int(__thiscall *)(void* thisPtr, void* sourceObjectPtr, char a3, int a4,
                      int a5, int16_t a6);
using AttachedEffectInitFn = int(__thiscall *)(void* thisPtr,
                                               void* ownerWidgetPtr, int a3,
                                               int a4, int16_t a5, int a6,
                                               int a7, unsigned int a8);
using AttachedEffectDirectAttachFn =
    unsigned int(__thiscall *)(void* thisPtr, void* ownerWidgetPtr,
                               int16_t attachPointIndex,
                               int attachPointArrayPtr,
                               unsigned int attachPointCount);
using AttachModelToPointFn =
    void(__fastcall *)(void* parentSpritePtr, int attachPointIndex,
                       void* childSpritePtr);
using CreateSpriteRuntimeFn = void *(__thiscall *)(void *thisPtr);
using SpriteFrameUpdateFn =
    int(__thiscall *)(int thisPtr, float dt, int a3, unsigned int a4, int a5);
using SpriteFrameLiteUpdateFn = int(__thiscall *)(int thisPtr, float dt);
using CreateGeosetFromRawArraysFn = void *(__fastcall *)(int, int, int, int, int,
                                                         int, int, int);
using RuntimeModelPlainCtorFn = void*(__thiscall *)(void* thisPtr, int a2);
using RuntimeModelComplexCtorFn = void*(__thiscall *)(void* thisPtr);
using ResolveRuntimeModelFromHandleFn = void*(__stdcall *)(void* handlePtr);
using PromoteRuntimeModelFn = void*(__thiscall *)(void* thisPtr);
using RuntimeInitFromModelDataFn = char*(__thiscall *)(char* thisPtr,
                                                       void* modelDataPtr);
using BuildChildRuntimeModelLinksFn =
    void*(__thiscall *)(void* thisPtr, void* modelDataPtr);
using RuntimePoseUpdateFn =
    int(__fastcall *)(int runtimeModel, const __m128i *poseMatrix, float scale,
                      int a4, int a5);
using RuntimeMatrixWriteFn =
    void(__fastcall *)(int nodePtr, int sourceMatrixPtr, int destMatrixPtr);
using RuntimeGroupPaletteWrapperFn = int(__fastcall *)(int runtimeModel,
                                                      int poseStackBasePtr);
using RuntimeSimpleGroupPaletteFn = void(__thiscall *)(int runtimeModel);
using RuntimePropagatePoseTreeFn = int(__fastcall *)(int runtimeModel, int a2);
using RuntimeRecurseChildTreeFn = void(__fastcall *)(int runtimeModel, int a2);
using RuntimeMatrixRangeCopyFn = int(__fastcall *)(int runtimeModel, int a2,
                                                   int a3);
using RuntimeMatrixFlushFn = int(__thiscall *)(int runtimeModel);
using RuntimeWriteSharedPresetOutputFn =
    const __m128i*(__fastcall *)(int contextPtr, int nodePtr);
using RuntimeWriteLocalPointOutputFn =
    int(__fastcall *)(int contextPtr, int nodePtr);
using RuntimeWritePrimaryPresetOutputFn =
    const __m128i*(__fastcall *)(int contextPtr, int nodePtr, float* a3);

struct HookConfig {
  bool enabled = false;
  bool logEnabled = false;
  bool poseEnabled = false;
  bool attachmentEnabled = false;
};

struct RuntimeChildLinkProbeRecord {
  void* linkNode = nullptr;
  void* childRuntimeModelPtr = nullptr;
  uint32_t tag = 0u;
  uint32_t sourceMeta = 0u;
  uint32_t bucketIndex = 0u;
};

struct ModelDataChildLinkProbeRecord {
  void* linkNode = nullptr;
  void* childModelDataPtr = nullptr;
  uint32_t sourceMeta = 0u;
  uint32_t bucketIndex = 0u;
};

struct RuntimeContextChildLinkProbeRecord {
  void* runtimeModelPtr = nullptr;
  uint32_t offset = 0u;
  uint32_t maxTag = 0u;
};

struct RuntimeParentLinkRecord {
  void* parentRuntimeModelPtr = nullptr;
  uint32_t sourceMeta = 0u;
  uint32_t bucketIndex = 0u;
  uint64_t lastSeenFrame = 0u;
};

struct AttachModelToPointScopeState {
  uint32_t depth = 0u;
  uint32_t callerRva = 0u;
  void* parentSpritePtr = nullptr;
  void* parentRuntimeModelPtr = nullptr;
  void* childSpritePtr = nullptr;
};

thread_local AttachModelToPointScopeState g_attachModelToPointScopeState = {};

struct AttachedEffectInitScopeState {
  uint32_t depth = 0u;
  void* effectPtr = nullptr;
  void* ownerWidgetPtr = nullptr;
};

thread_local AttachedEffectInitScopeState g_attachedEffectInitScopeState = {};

struct BuildChildRuntimeScopeState {
  uint32_t depth = 0u;
  void* parentRuntimeModelPtr = nullptr;
  void* parentModelDataPtr = nullptr;
};

thread_local BuildChildRuntimeScopeState g_buildChildRuntimeScopeState = {};

constexpr uintptr_t kCreateSpriteAndBindSourceObjectRva = 0x6BD110;
constexpr uintptr_t kAttachedEffectInitRva = 0x6BB2C0;
constexpr uintptr_t kAttachedEffectDirectAttachRva = 0x6B9FF0;
constexpr uintptr_t kAttachModelToPointRva = 0x184E50;
constexpr uintptr_t kCreateSpriteRuntimeRva = 0x185250;
constexpr uintptr_t kCreateGeosetFromRawArraysRva = 0x126250;
constexpr uintptr_t kRuntimeModelPlainCtorRva = 0x121880;
constexpr uintptr_t kRuntimeModelComplexCtorRva = 0x1219C0;
constexpr uintptr_t kResolveRuntimeModelFromHandleRva = 0x12A3C0;
constexpr uintptr_t kPromoteRuntimeModelRva = 0x12A5C0;
constexpr uintptr_t kRuntimeInitFromModelDataRva = 0x130D90;
constexpr uintptr_t kBuildChildRuntimeModelLinksRva = 0x131F60;
constexpr uintptr_t kBuildChildRuntimeModelLinksEndRva = 0x1320D0;
constexpr uintptr_t kCreateSpriteRuntimeCallerEndRva = 0x1859D0;
constexpr uintptr_t kPromoteRuntimeModelEndRva = kPromoteRuntimeModelRva + 0x93;
constexpr uintptr_t kSpriteMiniFrameUpdateRva = 0x1820C0;
constexpr uintptr_t kSpriteMiniFrameLiteUpdateRva = 0x1825E0;
constexpr uintptr_t kSpriteFrameUpdateRva = 0x182300;
constexpr uintptr_t kSpriteFrameLiteUpdateRva = 0x1826C0;
constexpr uintptr_t kRuntimePoseUpdateRva = 0x12F0A0;
constexpr uintptr_t kRuntimeMatrixWriteRva = 0x12E600;
constexpr uintptr_t kRuntimeGroupPaletteWrapperRva = 0x12FED0;
constexpr uintptr_t kRuntimeSimpleGroupPaletteRva = 0x12FF90;
constexpr uintptr_t kRuntimePropagatePoseTreeRva = 0x12F7E0;
constexpr uintptr_t kRuntimeRecurseChildTreeRva = 0x12EC90;
constexpr uintptr_t kRuntimeMatrixRangeCopyRva = 0x12FDC0;
constexpr uintptr_t kRuntimeMatrixFlushRva = 0x12FF50;
constexpr uintptr_t kRuntimeWriteSharedPresetOutputRva = 0x77DAA0;
constexpr uintptr_t kRuntimeWriteLocalPointOutputRva = 0x77DA20;
constexpr uintptr_t kRuntimeWritePrimaryPresetOutputRva = 0x77DF10;
constexpr size_t kSourceModelResourceOffset = 0x20; // this[8]
constexpr size_t kSourceFlagsOffset = 0x28;         // this[10]
constexpr size_t kRuntimeMatrixCountOffset = 0x5C;
constexpr size_t kRuntimeMatrixArrayOffset = 0x60;
constexpr size_t kRuntimeOverrideOutputBundleOffset = 0xFC;
constexpr size_t kRuntimeLocalPointOutputArrayOffset = 0xB4;
constexpr size_t kSpriteHostBoundSpriteOffset = 0x2C;
constexpr size_t kCModelRenderablePartCountOffset = 0x0C;
constexpr size_t kCModelRenderablePartArrayOffset = 0x10;
constexpr size_t kRenderablePartPaletteSlotOffset = 0x08;
constexpr size_t kRenderablePartGeosetDataOffset = 0x0C;
constexpr size_t kRenderablePartSkipFlagOffset = 0x10;
constexpr size_t kGeosetDataGroupCountOffset = 0xF0;
constexpr uint32_t kMaxRuntimeRenderablePartsForPaletteScan = 4096u;

std::atomic<bool> g_active{false};
std::atomic<bool> g_bootstrapHooksInstalled{false};
std::atomic<bool> g_fullHooksInstalled{false};
std::atomic<uint32_t> g_bindLogCount{0};
std::atomic<uint32_t> g_poseLogCount{0};
std::atomic<uint32_t> g_spriteFrameLogCount{0};

CreateSpriteAndBindSourceObjectFn g_trampolineCreateSpriteAndBindSourceObject =
    nullptr;
AttachedEffectInitFn g_trampolineAttachedEffectInit = nullptr;
AttachedEffectDirectAttachFn g_trampolineAttachedEffectDirectAttach = nullptr;
AttachModelToPointFn g_trampolineAttachModelToPoint = nullptr;
CreateSpriteRuntimeFn g_trampolineCreateSpriteRuntime = nullptr;
CreateGeosetFromRawArraysFn g_trampolineCreateGeosetFromRawArrays = nullptr;
RuntimeModelPlainCtorFn g_trampolineRuntimeModelPlainCtor = nullptr;
RuntimeModelComplexCtorFn g_trampolineRuntimeModelComplexCtor = nullptr;
ResolveRuntimeModelFromHandleFn g_trampolineResolveRuntimeModelFromHandle =
    nullptr;
PromoteRuntimeModelFn g_trampolinePromoteRuntimeModel = nullptr;
RuntimeInitFromModelDataFn g_trampolineRuntimeInitFromModelData = nullptr;
BuildChildRuntimeModelLinksFn g_trampolineBuildChildRuntimeModelLinks = nullptr;
SpriteFrameUpdateFn g_trampolineSpriteMiniFrameUpdate = nullptr;
SpriteFrameLiteUpdateFn g_trampolineSpriteMiniFrameLiteUpdate = nullptr;
SpriteFrameUpdateFn g_trampolineSpriteFrameUpdate = nullptr;
SpriteFrameLiteUpdateFn g_trampolineSpriteFrameLiteUpdate = nullptr;
RuntimePoseUpdateFn g_trampolineRuntimePoseUpdate = nullptr;
RuntimeMatrixWriteFn g_trampolineRuntimeMatrixWrite = nullptr;
RuntimeGroupPaletteWrapperFn g_trampolineRuntimeGroupPaletteWrapper = nullptr;
RuntimeSimpleGroupPaletteFn g_trampolineRuntimeSimpleGroupPalette = nullptr;
RuntimePropagatePoseTreeFn g_trampolineRuntimePropagatePoseTree = nullptr;
RuntimeRecurseChildTreeFn g_trampolineRuntimeRecurseChildTree = nullptr;
RuntimeMatrixRangeCopyFn g_trampolineRuntimeMatrixRangeCopy = nullptr;
RuntimeMatrixFlushFn g_trampolineRuntimeMatrixFlush = nullptr;
RuntimeWriteSharedPresetOutputFn g_trampolineRuntimeWriteSharedPresetOutput =
    nullptr;
RuntimeWriteLocalPointOutputFn g_trampolineRuntimeWriteLocalPointOutput =
    nullptr;
RuntimeWritePrimaryPresetOutputFn g_trampolineRuntimeWritePrimaryPresetOutput =
    nullptr;
HookConfig g_config = {};
uintptr_t g_gameBase = 0u;

// 混合调色板缓存 — Hook_RuntimeMatrixWrite 在 CGeosetData_BuildGroupBlendedPalette
// 写入后当场捕获。slotIndex 从 outputPalette 地址反算：(out - dword_6FBC6BD0)/48。
//
// Phase 7.31 Iteration E：性能修复。
// 原实现是 `std::unordered_map<uint32_t, BlendedPaletteEntry>` +
// `std::vector<Matrix4>`，P0 让每帧一次性写入 14K+ slot 后哈希表和 vector
// 分配把 Populate 路径压到 94 ms 单帧。改成固定大小开放寻址数组：
//   - 容量 64K（slotIndex 上限已由 PublishCurrentDrawContract 检查 < 0x3A98
//     ≈ 14996，小于 16K；预留 64K 给未来扩展。）
//   - 直接用 slotIndex 作数组 index，省掉哈希。
//   - Matrix4 内嵌到 entry，去掉 vector 的堆分配。
// 查询和写入都是 O(1) 数组访问，没有锁和分配。
static constexpr uint32_t kRuntimeMatrixBatchMaxCount = 256u;
struct BlendedPaletteEntry {
  Matrix4 matrix = Matrix4{};
  uint32_t frameTag = 0u;
  uint64_t writeSerial = 0u;
  bool valid = false;
};
// 64K entries × (64 + 8 + 1) ≈ 4.6 MB 常驻，换来热路径 O(1) 写。
static constexpr uint32_t kSlotBlendedPaletteCacheSize = 65536u;
static std::array<BlendedPaletteEntry, kSlotBlendedPaletteCacheSize>
    s_slotBlendedPaletteCache = {};
static std::atomic<uint64_t> s_slotBlendedPaletteWriteSerial{0u};

enum class RuntimeGroupPaletteProducerKind : uint32_t {
  Unknown = 0u,
  AllocAndFillWrapper = 1u,
  SimpleFallback = 2u,
};

struct RenderablePartPaletteBindingEntry {
  std::atomic<uintptr_t> renderablePart{0u};
  std::atomic<uint32_t> paletteSlotIndex{0xFFFFFFFFu};
  std::atomic<uint32_t> groupCount{0u};
  std::atomic<uint32_t> frameTag{0u};
  std::atomic<uint32_t> producerKind{0u};
  std::atomic<uint64_t> writeSerial{0u};
  std::atomic<uint64_t> paletteWriteSerial{0u};
  std::atomic<uint32_t> paletteCount{0u};
  std::atomic<uint32_t> paletteFrameTag{0u};
  std::atomic<uint64_t> paletteHash{0u};
  // Phase 7.51：记录这个 renderablePart 属于哪个 runtimeModel（即 0x12FED0 的
  // this 参数）。用于在 submit 时通过 renderablePart 反查到 producer 侧真正的
  // runtimeModel key，再去 PoseRegistry 查 final-pose；这比传入的
  // packet.renderable.runtimeModelPtr 更可信（后者是 alias 解析后的值，1.27a
  // 上经常偏移错了）。
  std::atomic<uintptr_t> runtimeModel{0u};
  std::array<Matrix4, 64> palette{};
};

static constexpr size_t kRenderablePartPaletteBindingCacheSize = 8192u;
static constexpr uint32_t kRenderablePartPaletteSnapshotMaxCount = 64u;
static std::array<RenderablePartPaletteBindingEntry,
                  kRenderablePartPaletteBindingCacheSize>
    s_renderablePartPaletteBindings = {};
static std::atomic<uint64_t> s_renderablePartPaletteBindingSerial{0u};
static std::atomic<uint64_t> s_renderablePartPaletteSnapshotSerial{0u};
static std::atomic<uintptr_t> s_cachedGlobalPaletteBufBase{0u};

bool TryReadCurrentPaletteFrameTag(uint32_t& outFrameTag) {
  // Phase 7.31 Iteration F：避免每次调用都 syscall GetModuleHandleA。
  // g_gameBase 在 hook 安装时已缓存。
  outFrameTag = 0u;
  if (g_gameBase == 0u)
    return false;
  return SafeReadU32Fast(reinterpret_cast<const void*>(g_gameBase + 0xBDA4CCu),
                         0u, outFrameTag);
}

class SemanticHookPerfScope {
public:
  explicit SemanticHookPerfScope(render::SemanticDataPerfTag tag)
      : m_aggregateTag(tag), m_detailTag(tag),
        m_start(std::chrono::steady_clock::now()) {}

  SemanticHookPerfScope(render::SemanticDataPerfTag aggregateTag,
                        render::SemanticDataPerfTag detailTag)
      : m_aggregateTag(aggregateTag), m_detailTag(detailTag),
        m_start(std::chrono::steady_clock::now()) {}

  ~SemanticHookPerfScope() {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - m_start)
                             .count();
    const uint64_t elapsedUs =
        elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0u;
    render::NoteSemanticDataPerf(m_aggregateTag, elapsedUs);
    if (m_detailTag != m_aggregateTag)
      render::NoteSemanticDataPerf(m_detailTag, elapsedUs);
  }

private:
  render::SemanticDataPerfTag m_aggregateTag;
  render::SemanticDataPerfTag m_detailTag;
  std::chrono::steady_clock::time_point m_start;
};

std::atomic<uint64_t> g_spriteHostBindCount{0u};
std::atomic<uint64_t> g_runtimeModelCtorCount{0u};
std::atomic<uint64_t> g_runtimeModelComplexCtorCount{0u};
std::atomic<uint64_t> g_runtimeModelPlainCtorCount{0u};
std::atomic<uint64_t> g_runtimeModelCtorCallerPromoteCount{0u};
std::atomic<uint64_t> g_runtimeModelCtorCallerOtherCount{0u};
std::atomic<uint64_t> g_runtimeModelCreateCount{0u};
std::atomic<uint64_t> g_runtimeModelResolveCount{0u};
std::atomic<uint64_t> g_runtimeModelResolveResolvedIdentityCount{0u};
std::atomic<uint64_t> g_runtimeModelCreateCallerBuildChildLinksCount{0u};
std::atomic<uint64_t> g_runtimeModelCreateCallerCreateSpriteRuntimeCount{0u};
std::atomic<uint64_t> g_runtimeModelCreateCallerOtherCount{0u};
std::atomic<uint64_t> g_runtimeModelInitCopyCount{0u};
std::atomic<uint64_t> g_runtimeModelInitCopyPublishedFallbackCount{0u};
std::atomic<uint64_t> g_runtimeChildLinkBuildCount{0u};
std::atomic<uint64_t> g_runtimeChildLinkBuiltChildCount{0u};
std::atomic<uint64_t> g_runtimeChildBuildTimeDirectPublishCount{0u};
std::atomic<uint64_t> g_runtimeChildBuildTimeDirectPublishWithResourceCount{0u};
std::atomic<uint64_t> g_runtimeChildBuildModelDataPreLinkCount{0u};
std::atomic<uint64_t> g_runtimeChildBuildModelDataPostLinkCount{0u};
std::atomic<uint64_t> g_runtimeChildBuildModelDataPreUnreadableLinkCount{0u};
std::atomic<uint64_t> g_runtimeChildBuildModelDataPostUnreadableLinkCount{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyCount{0u};
std::atomic<uint64_t> g_runtimeMatrixFlushCount{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherPaletteReadyCount{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherPoseRevision{0u};
std::atomic<uint64_t> g_runtimePoseUpdatePalettePublishCount{0u};
std::atomic<uint64_t> g_runtimePoseUpdateLastRuntimeModelPtr{0u};
std::atomic<uint64_t> g_runtimePoseUpdateLastMatrixCount{0u};
std::atomic<uint64_t> g_runtimePoseUpdateLastMatrixHash{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWritePublishCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteMissCount{0u};
// Phase 7.31 P0：批量捕获 CGeosetData_BuildGroupBlendedPalette 结果。
// BatchCaptured 累积"被真正缓存的 slot 数"（等于 sum of groupCount），
// BatchOverflow 是 groupCount > 256 触发裁剪的次数，
// BatchUnreadable 是目标地址 +count*48 不可读时拒绝写入的次数，
// BatchLastGroupCount 记录最近一次 hook 看到的 count。
std::atomic<uint64_t> g_runtimeMatrixWriteBatchCapturedCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteBatchOverflowCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteBatchUnreadableCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteBatchLastGroupCount{0u};
std::atomic<uint64_t> g_runtimeGroupPaletteWrapperCallCount{0u};
std::atomic<uint64_t> g_runtimeGroupPaletteWrapperPartCount{0u};
std::atomic<uint64_t> g_runtimeGroupPaletteWrapperBindingCount{0u};
std::atomic<uint64_t> g_runtimeSimpleGroupPaletteCallCount{0u};
std::atomic<uint64_t> g_runtimeSimpleGroupPaletteSlotCapturedCount{0u};
std::atomic<uint64_t> g_runtimeSimpleGroupPaletteSlotUnreadableCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteBindingQueryHitCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteBindingQueryMissCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteSnapshotCapturedCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteSnapshotTooLargeCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteSnapshotUnreadableCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteSnapshotQueryHitCount{0u};
std::atomic<uint64_t> g_renderablePartPaletteSnapshotQueryMissCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteLastRuntimeModelPtr{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteLastMatrixIndex{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteLastMatrixCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteLastMatrixHash{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyPalettePublishHitCount{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyPalettePublishMissCount{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyPaletteFallbackCModelCount{0u};
// Phase 7.34 A3 优化：per-runtimeModel hash 稳定跳过计数。
// 高值说明大部分 range-copy 调用落在"连续帧 palette 不变"的情况，
// 快退路径消除了重复 PoseRegistry 录入开销。
std::atomic<uint64_t> g_runtimeMatrixRangeCopyPublishSkippedDedupCount{0u};
std::atomic<uint64_t> g_runtimeMatrixFlushPaletteSuppressedCount{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyLastRuntimeModelPtr{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyLastContextPtr{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyLastSourceBasePtr{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyLastMatrixCount{0u};
std::atomic<uint64_t> g_runtimeMatrixRangeCopyLastMatrixHash{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentRootHitCount{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentOwnerHitCount{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentChildHitCount{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentAliasHitCount{0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentRootPaletteReadyCount{
    0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount{
    0u};
std::atomic<uint64_t> g_runtimeMatrixPublisherAttachmentChildPaletteReadyCount{
    0u};

struct RuntimePoseArrayRange {
  uintptr_t runtimeModel = 0u;
  uintptr_t matrixArray = 0u;
  uint32_t matrixCount = 0u;
};

// Phase 7.79：从 std::mutex 切到 shared_mutex。
// `TryFindRuntimePoseArrayRangeForMatrix` 在 hot path（Hook_RuntimeMatrixWrite）
// 每帧 13K-30K 次访问，TLS hot cache miss 时进入 read 路径。注册写入路径
// （RegisterRuntimePoseArrayRange / clear）远少于查询读。reader 走 shared_lock，
// 让多读者并发；writer 走 unique_lock，互斥语义保持原状。
std::shared_mutex g_runtimePoseArrayRangeMutex;
std::unordered_map<uintptr_t, RuntimePoseArrayRange> g_runtimePoseArrayByModel;
std::unordered_map<uintptr_t, RuntimePoseArrayRange>
    g_runtimePoseArrayByMatrixPtr;
// Phase 7.95：registry size 原子计数，让 TryFind 的 empty-check 快路径生效。
std::atomic<uint32_t> g_runtimePoseArrayRegistrySize{0u};

std::atomic<uint64_t> g_attachmentChildLineageBootstrapAttemptCount{0u};
std::atomic<uint64_t> g_attachmentChildLineageBootstrapSuccessCount{0u};
std::atomic<uint64_t>
    g_attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount{0u};
std::atomic<uint64_t>
    g_attachmentChildLineageBootstrapMissNoModelDataLinksCount{0u};
std::atomic<uint64_t>
    g_attachmentChildLineageBootstrapMissNoUniqueChildCount{0u};
std::atomic<uint64_t> g_attachmentAncestorIdentityHintWriteCount{0u};
std::atomic<uint64_t> g_sourceObjectRenderBridgeResolvedByEntryCount{0u};
std::atomic<uint64_t> g_sourceObjectRenderBridgeResolvedBySceneNodeCount{0u};
std::atomic<uint64_t> g_spriteHostBindResolvedIdentityCount{0u};
std::atomic<uint64_t> g_spriteHostBindResolvedUnitCount{0u};
std::atomic<uint64_t> g_spriteHostBindResolvedHandleCount{0u};
std::atomic<uint64_t> g_spriteHostBindResolvedRawcodeCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceHintCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceResolvedIdentityCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceResolvedUnitCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceResolvedHandleCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceResolvedRawcodeCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceBaseAliasPublishCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceDeepIdentityResolvedCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceObjectRuntimeFieldCandidateCount{0u};
std::atomic<uint64_t> g_spriteFrameSourceObjectRegistryFieldHitCount{0u};
std::atomic<uint64_t> g_spriteFramePoseBaseAliasPublishCount{0u};
std::atomic<uint64_t> g_spriteFramePoseBaseAliasMatrixPaletteCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentRootRuntimeHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentOwnerRuntimeHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentChildRuntimeHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentContextHintCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentFullUpdateHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentLiteUpdateHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentCallerKnownCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentCallerChangedCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentAttachScopeHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentAttachScopeOwnerHitCount{0u};
std::atomic<uint64_t> g_spriteFrameAttachmentAttachScopeParentRuntimeMatchCount{0u};
std::atomic<uint64_t> g_attachedEffectInitBindCount{0u};
std::atomic<uint64_t> g_attachedEffectInitResolvedIdentityCount{0u};
std::atomic<uint64_t> g_attachedEffectInitResolvedUnitCount{0u};
std::atomic<uint64_t> g_attachedEffectInitResolvedHandleCount{0u};
std::atomic<uint64_t> g_attachedEffectInitResolvedRawcodeCount{0u};
std::atomic<uint64_t> g_attachedEffectInitParentRuntimeOwnerPublishCount{0u};
std::atomic<uint64_t> g_attachedEffectDirectBindCount{0u};
std::atomic<uint64_t> g_attachedEffectDirectResolvedIdentityCount{0u};
std::atomic<uint64_t> g_attachedEffectDirectResolvedUnitCount{0u};
std::atomic<uint64_t> g_attachedEffectDirectResolvedHandleCount{0u};
std::atomic<uint64_t> g_attachedEffectDirectResolvedRawcodeCount{0u};
std::atomic<uint64_t> g_attachModelToPointBindCount{0u};
std::atomic<uint64_t> g_attachModelToPointResolvedIdentityCount{0u};
std::atomic<uint64_t> g_attachModelToPointResolvedUnitCount{0u};
std::atomic<uint64_t> g_attachModelToPointResolvedHandleCount{0u};
std::atomic<uint64_t> g_attachModelToPointResolvedRawcodeCount{0u};
std::atomic<uint64_t>
    g_attachModelToPointPromotedAttachmentChildRuntimeCount{0u};
std::atomic<uint64_t>
    g_attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount{0u};
std::atomic<uint64_t> g_currentRenderIdentityHintCount{0u};
std::atomic<uint64_t> g_currentRenderIdentityResolvedCount{0u};
std::atomic<uint64_t> g_sourceObjectIdentityHintResolvedCount{0u};
std::atomic<uint64_t> g_runtimeSourceObjectPublishCount{0u};
std::atomic<uint64_t> g_attachmentRigidPublishedWithSourceObjectCount{0u};
std::atomic<uint64_t> g_attachmentRigidSourceObjectFromChildRuntimeCount{0u};
std::atomic<uint64_t> g_attachmentRigidSourceObjectFromOwnerRuntimeCount{0u};
std::atomic<uint64_t> g_attachmentRigidSourceObjectFromRootRuntimeCount{0u};
std::atomic<uint64_t> g_overrideOutputSampleFrame{0u};
std::atomic<uint64_t> g_overrideOutputLastActiveFrame{0u};
std::atomic<uint64_t> g_overridePrimaryPresetWriteCount{0u};

// Phase 7.47 dt gate probe（纯诊断，极低开销）：
//   这四个原子 counter 记录 CSpriteUber_PreRenderAndUpdatePosePalette_*
//   的 dt 分布。它们与 `kWar3RuntimeConfigInstallSpriteFrameHooksWithoutPose`
//   无关——dt probe 走独立 minhook 安装路径。
std::atomic<uint64_t> g_spriteUberPreRenderTotalCount{0u};
std::atomic<uint64_t> g_spriteUberPreRenderDtZeroCount{0u};
std::atomic<uint64_t> g_spriteUberPreRenderDtBelowEpsilonCount{0u};
std::atomic<uint64_t> g_spriteUberPreRenderDtPositiveCount{0u};
std::atomic<uint64_t> g_spriteUberPreRenderDtNegativeCount{0u};
std::atomic<uint32_t> g_spriteUberPreRenderLastDtBits{0u};
std::atomic<uint32_t> g_spriteUberPreRenderLastZeroDtFrameTag{0u};
std::atomic<uint32_t> g_spriteUberPreRenderLastPositiveDtFrameTag{0u};

// Per-frameTag 去重计数：writer 在一个 palette frameTag 里首次触发时累加。
// 让我们可以和 full trace 的每帧 `frameTag` 对齐，判断冻结窗口里
// writer 到底有没有跑。
std::atomic<uint32_t> g_runtimeMatrixWriteLastFrameTag{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteFramesWithHitCount{0u};
std::atomic<uint64_t> g_runtimeMatrixWriteFramesEmptyCount{0u};
std::atomic<uint32_t> g_runtimeGroupPaletteWrapperLastFrameTag{0u};
std::atomic<uint64_t> g_runtimeGroupPaletteWrapperFramesWithHitCount{0u};
std::atomic<uint64_t> g_runtimeGroupPaletteWrapperFramesEmptyCount{0u};
std::atomic<uint32_t> g_runtimeSimpleGroupPaletteLastFrameTag{0u};
std::atomic<uint64_t> g_runtimeSimpleGroupPaletteFramesWithHitCount{0u};
std::atomic<uint64_t> g_runtimeSimpleGroupPaletteFramesEmptyCount{0u};

// 上一次见到的 palette frameTag（用于"writer 静默帧"统计的发现逻辑）。
std::atomic<uint32_t> g_paletteFrameTagLastSeen{0u};
std::atomic<uint64_t> g_paletteFrameTagAdvanceCount{0u};
std::atomic<uint64_t> g_overrideSharedPresetWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointNonZeroWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointObservedChildLinkWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointMatchedChildLinkWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointMatchedChildPaletteReadyWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointMatchedChildLinkBySourceRecordWriteCount{
    0u};
std::atomic<uint64_t>
    g_overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointContextRuntimeWithChildLinksWriteCount{
    0u};
std::atomic<uint64_t> g_overrideLocalPointContextMatchedChildLinkWriteCount{0u};
std::atomic<uint64_t>
    g_overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount{0u};
std::atomic<uint64_t>
    g_overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount{
        0u};
std::atomic<uint64_t>
    g_overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointScratchRootMatchedChildLinkWriteCount{
    0u};
std::atomic<uint64_t>
    g_overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount{0u};
std::atomic<uint64_t>
    g_overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount{
        0u};
std::atomic<uint64_t> g_overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount{
    0u};
std::atomic<uint64_t> g_overrideLocalPointArgBlockMatchedChildLinkWriteCount{
    0u};
std::atomic<uint64_t>
    g_overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointArgBlockIdentityHintWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount{
    0u};
std::atomic<uint64_t> g_overrideLocalPointArg4BlockMatchedChildLinkWriteCount{
    0u};
std::atomic<uint64_t>
    g_overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointArg4BlockIdentityHintWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointChildSourceMetaIdentityHintWriteCount{
    0u};
std::atomic<uint64_t> g_overrideLocalPointSpriteBoundCandidateWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointParentSpriteIdentityHintWriteCount{
    0u};
std::atomic<uint64_t> g_overrideLocalPointRootRuntimeHitWriteCount{0u};
std::atomic<uint64_t> g_overrideLocalPointRootRuntimeWithChildLinksWriteCount{
    0u};
std::atomic<uint64_t> g_overrideLocalPointRootRuntimeMatchedChildLinkWriteCount{
    0u};
std::atomic<uint64_t>
    g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount{0u};
std::atomic<uint64_t>
    g_overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount{
        0u};
std::atomic<uint64_t>
    g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount{
        0u};
std::atomic<uint64_t> g_attachmentRigidPublishedCount{0u};
std::atomic<uint32_t> g_overrideMaxPrimaryPresetSlotIndex{0u};
std::atomic<uint32_t> g_overrideMaxSharedPresetSlotIndex{0u};
std::atomic<uint32_t> g_overrideMaxLocalPointSlotIndex{0u};
std::atomic<uint32_t> g_overrideMaxObservedChildLinkCount{0u};
std::atomic<uint32_t> g_overrideMaxObservedChildLinkTag{0u};
std::atomic<uint64_t> g_overrideLastPrimaryPresetHash{0u};
std::atomic<uint64_t> g_overrideLastSharedPresetHash{0u};
std::atomic<uint64_t> g_overrideLastRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastMatchedChildRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastMatchedChildBySourceRecordRuntimeModelPtr{
    0u};
std::atomic<uint64_t> g_overrideLastContextRuntimeWithChildLinksPtr{0u};
std::atomic<uint64_t> g_overrideLastScratchRootPtr{0u};
std::atomic<uint64_t> g_overrideLastScratchRootRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastArgBlockPtr{0u};
std::atomic<uint64_t> g_overrideLastArgBlockRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastArgBlockIdentityHintPtr{0u};
std::atomic<uint64_t> g_overrideLastArg4BlockPtr{0u};
std::atomic<uint64_t> g_overrideLastArg4BlockRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastArg4BlockIdentityHintPtr{0u};
std::atomic<uint64_t> g_overrideLastChildSourceMetaPtr{0u};
std::atomic<uint64_t> g_overrideLastChildSourceMetaRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastSpriteBoundCandidateSpritePtr{0u};
std::atomic<uint64_t> g_overrideLastSpriteBoundCandidateRuntimeModelPtr{0u};
std::atomic<uint64_t> g_overrideLastParentSpriteIdentityHintSpritePtr{0u};
std::atomic<uint64_t> g_overrideLastParentSpriteIdentityHintRuntimeModelPtr{
    0u};
std::atomic<uint64_t> g_overrideLastRootRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastSourceObjectRenderBridgeSourceObjectPtr{0u};
std::atomic<uint64_t> g_lastSourceObjectRenderBridgeSceneNodePtr{0u};
std::atomic<uint64_t> g_lastSourceObjectIdentityHintSourceObjectPtr{0u};
std::atomic<uint64_t> g_lastSourceObjectIdentityHintCandidatePtr{0u};
std::atomic<uint64_t> g_lastSpriteHostSourceObjectPtr{0u};
std::atomic<uint64_t> g_lastSpriteHostSpritePtr{0u};
std::atomic<uint64_t> g_lastSpriteHostRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastSpriteHostUnitPtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceObjectPtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceBaseRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceObjectVtablePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceObjectSceneNodeCandidatePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceObjectSpriteCandidatePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceObjectRegistryFieldCandidatePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceDeepIdentityCandidatePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceWorldObjectEntryPtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceSceneNodePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameSourceUnitPtr{0u};
std::atomic<uint64_t> g_lastSpriteFramePoseBaseRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastSpriteFramePoseBaseMatrixCount{0u};
std::atomic<uint64_t> g_lastSpriteFrameAttachmentSpritePtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameAttachmentRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastSpriteFrameAttachmentContextPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectInitOwnerWidgetPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectInitChildSpritePtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectInitChildRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectInitParentRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectInitUnitPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectDirectOwnerWidgetPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectDirectChildSpritePtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectDirectChildRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastAttachedEffectDirectUnitPtr{0u};
std::atomic<uint64_t> g_lastAttachModelToPointParentSpritePtr{0u};
std::atomic<uint64_t> g_lastAttachModelToPointChildSpritePtr{0u};
std::atomic<uint64_t> g_lastAttachModelToPointChildRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastAttachModelToPointPromotedOwnerRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastAttachModelToPointPromotedChildRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastAttachModelToPointPromotedChildModelResourcePtr{0u};
std::atomic<uint64_t> g_lastAttachModelToPointUnitPtr{0u};
std::atomic<uint64_t> g_lastAttachScopeParentSpritePtr{0u};
std::atomic<uint64_t> g_lastAttachScopeParentRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastAttachScopeChildSpritePtr{0u};
std::atomic<uint64_t> g_lastAttachScopeChildRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastAttachScopeHitRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastCurrentRenderIdentityWorldObjectEntryPtr{0u};
std::atomic<uint64_t> g_lastCurrentRenderIdentitySceneNodePtr{0u};
std::atomic<uint64_t> g_lastCurrentRenderIdentityUnitPtr{0u};
std::atomic<uint64_t> g_lastRuntimeSourceObjectPtr{0u};
std::atomic<uint64_t> g_lastRuntimeSourceSpriteObjectPtr{0u};
std::atomic<uint64_t> g_lastRuntimeSourceRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelResolveRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelResolveHandlePtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelCtorRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelCreateRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelCreateModelDataPtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelInitRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeModelInitModelDataPtr{0u};
std::atomic<uint64_t> g_lastAttachmentRigidSourceObjectPtr{0u};
std::atomic<uint64_t> g_lastAttachmentRigidSourceSpriteObjectPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildLinkBuildParentRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildLinkBuildChildRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildLinkBuildModelDataPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildTimeDirectParentModelDataPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildTimeDirectRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildTimeDirectModelDataPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildTimeDirectModelResourcePtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildModelDataParentRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildModelDataPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildModelDataGroupRecordsPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildModelDataHeadPtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildModelDataLinkNodePtr{0u};
std::atomic<uint64_t> g_lastRuntimeChildBuildModelDataChildModelDataPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeChildBuildModelDataChildModelResourcePtr{0u};
std::atomic<uint64_t> g_lastRuntimeMatrixPublisherRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeMatrixPublisherMatchedRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastRuntimeMatrixPublisherMatrixCount{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapParentRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapChildRuntimeModelPtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapParentModelDataPtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapChildModelDataPtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapChildModelResourcePtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr{0u};
std::atomic<uint64_t>
    g_lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr{0u};
std::atomic<uint64_t> g_lastAttachmentAncestorFromRuntimeModelPtr{0u};
std::atomic<uint64_t> g_lastAttachmentAncestorRuntimeModelPtr{0u};
std::atomic<uint32_t> g_overrideLastLocalPointSlotIndex{0u};
std::atomic<uint32_t> g_overrideLastLocalPointSourceRecordIndex{0u};
std::atomic<uint32_t> g_overrideLastObservedChildLinkCount{0u};
std::atomic<uint32_t> g_overrideLastMatchedChildLinkCount{0u};
std::atomic<uint32_t> g_overrideLastMatchedChildMatrixCount{0u};
std::atomic<uint32_t> g_overrideLastMatchedChildBySourceRecordLinkCount{0u};
std::atomic<uint32_t> g_overrideLastMatchedChildBySourceRecordMatrixCount{0u};
std::atomic<uint32_t> g_overrideLastContextRuntimeWithChildLinksOffset{0u};
std::atomic<uint32_t> g_overrideLastContextRuntimeWithChildLinksCount{0u};
std::atomic<uint32_t> g_overrideLastContextRuntimeWithChildLinksMaxTag{0u};
std::atomic<uint32_t> g_overrideLastScratchRootRuntimeChildLinkCount{0u};
std::atomic<uint32_t> g_overrideLastScratchRootRuntimeMaxTag{0u};
std::atomic<uint32_t> g_overrideLastArgBlockRuntimeOffset{0u};
std::atomic<uint32_t> g_overrideLastArgBlockRuntimeChildLinkCount{0u};
std::atomic<uint32_t> g_overrideLastArgBlockRuntimeMaxTag{0u};
std::atomic<uint32_t> g_overrideLastArgBlockIdentityHintOffset{0u};
std::atomic<uint32_t> g_overrideLastArg4BlockRuntimeOffset{0u};
std::atomic<uint32_t> g_overrideLastArg4BlockRuntimeChildLinkCount{0u};
std::atomic<uint32_t> g_overrideLastArg4BlockRuntimeMaxTag{0u};
std::atomic<uint32_t> g_overrideLastArg4BlockIdentityHintOffset{0u};
std::atomic<uint32_t> g_overrideLastRootRuntimeChildLinkCount{0u};
std::atomic<uint32_t> g_overrideLastRootRuntimeMaxTag{0u};
std::atomic<uint32_t> g_lastSpriteHostJHandle{0u};
std::atomic<uint32_t> g_lastSpriteHostRawcode{0u};
std::atomic<uint32_t> g_lastSpriteFrameSourceJHandle{0u};
std::atomic<uint32_t> g_lastSpriteFrameSourceRawcode{0u};
std::atomic<uint32_t> g_lastSpriteFrameSourceObjectRuntimeFieldOffset{0u};
std::atomic<uint32_t> g_lastSpriteFrameSourceObjectRegistryFieldOffset{0u};
std::atomic<uint32_t> g_lastSpriteFrameSourceDeepIdentityOffset{0u};
std::atomic<uint32_t> g_lastSpriteFrameAttachmentRoleMask{0u};
std::atomic<uint32_t> g_lastSpriteFrameAttachmentUpdateKind{0u};
std::atomic<uint32_t> g_lastSpriteFrameAttachmentCallerRva{0u};
std::atomic<uint32_t> g_lastSourceObjectIdentityHintOffset{0u};
std::atomic<uint32_t> g_lastAttachedEffectInitJHandle{0u};
std::atomic<uint32_t> g_lastAttachedEffectInitRawcode{0u};
std::atomic<uint32_t> g_lastAttachedEffectDirectJHandle{0u};
std::atomic<uint32_t> g_lastAttachedEffectDirectRawcode{0u};
std::atomic<uint32_t> g_lastAttachModelToPointJHandle{0u};
std::atomic<uint32_t> g_lastAttachModelToPointRawcode{0u};
std::atomic<uint32_t> g_lastAttachModelToPointAttachPointIndex{0u};
std::atomic<uint32_t> g_lastAttachScopeCallerRva{0u};
std::atomic<uint32_t> g_lastAttachScopeHitRoleMask{0u};
std::atomic<uint32_t> g_lastRuntimeModelCtorCallerRva{0u};
std::atomic<uint32_t> g_lastRuntimeModelCtorKind{0u};
std::atomic<uint32_t> g_lastRuntimeModelResolveCallerRva{0u};
std::atomic<uint32_t> g_lastRuntimeModelCreateCallerRva{0u};
std::atomic<uint32_t> g_lastRuntimeModelInitCallerRva{0u};
std::atomic<uint32_t> g_lastRuntimeChildLinkBuildSourceMeta{0u};
std::atomic<uint32_t> g_lastRuntimeChildBuildModelDataPhase{0u};
std::atomic<uint32_t> g_lastRuntimeChildBuildModelDataGroupCount{0u};
std::atomic<uint32_t> g_lastRuntimeChildBuildModelDataLinkCount{0u};
std::atomic<uint32_t> g_lastRuntimeChildBuildModelDataUnreadableLinkCount{0u};
std::atomic<uint32_t> g_lastRuntimeChildBuildModelDataSourceMeta{0u};
std::atomic<uint32_t> g_lastRuntimeMatrixPublisherKind{0u};
std::atomic<uint32_t> g_lastRuntimeMatrixPublisherRoleMask{0u};
std::atomic<uint32_t> g_lastAttachmentChildLineageBootstrapSourceMeta{0u};
std::atomic<uint32_t> g_lastAttachmentChildLineageBootstrapBucketIndex{0u};
std::atomic<uint32_t> g_lastAttachmentChildLineageBootstrapModelDataLinkCount{
    0u};
std::atomic<uint32_t> g_lastAttachmentChildLineageBootstrapRuntimeLinkCount{0u};
std::atomic<uint32_t>
    g_lastAttachmentChildLineageBootstrapStrictCandidateCount{0u};
std::atomic<uint32_t>
    g_lastAttachmentChildLineageBootstrapSourceCandidateCount{0u};
std::atomic<uint32_t>
    g_lastAttachmentChildLineageBootstrapBucketCandidateCount{0u};
std::atomic<uint32_t> g_lastAttachmentChildLineageBootstrapAllCandidateCount{
    0u};
std::atomic<uint32_t>
    g_lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal{0u};
std::atomic<uint32_t>
    g_lastAttachmentChildLineageBootstrapModelDataBucketCount{0u};
std::atomic<uint32_t> g_lastAttachmentAncestorDepth{0u};
std::atomic<uint32_t> g_overrideLastLocalPointXBits{0u};
std::atomic<uint32_t> g_overrideLastLocalPointYBits{0u};
std::atomic<uint32_t> g_overrideLastLocalPointZBits{0u};

std::mutex g_runtimeParentLinkMutex;
std::unordered_map<void*, RuntimeParentLinkRecord> g_runtimeParentLinks;
std::mutex g_runtimePaletteTreeDedupeMutex;
uint64_t g_runtimePaletteTreeDedupeFrame = 0u;
std::unordered_set<void*> g_runtimePaletteTreeDedupeRoots;
std::unordered_set<void*> g_runtimePaletteTreeDedupeOwnerRoots;

bool GetEnvBoolCached(const char *name, bool defaultValue) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return defaultValue;
  return value != "0";
}

uint64_t HashMatrixPalette(const std::vector<Matrix4>& matrices) {
  uint64_t hash = 1469598103934665603ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(matrices.data());
  const size_t size = matrices.size() * sizeof(Matrix4);
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t HashBytes(const void* data, size_t size) {
  if (data == nullptr || size == 0u)
    return 0ull;

  uint64_t hash = 1469598103934665603ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

void* TryResolveDirectModelResourceFromRuntimeModel(void* runtimeModelPtr);
void* NormalizeDirectModelResourcePtr(void* modelResourcePtr);

template <typename T>
void UpdateAtomicMax(std::atomic<T>& target, T value) {
  T current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed)) {
  }
}

uint32_t FloatBits(float value) {
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float FloatFromBits(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uintptr_t GetCallReturnAddress() {
#if defined(_MSC_VER)
  return reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
  return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
}

uint32_t GetModuleRvaFromAddress(uintptr_t address) {
  if (g_gameBase == 0u || address < g_gameBase)
    return 0u;
  const uintptr_t rva = address - g_gameBase;
  return rva <= 0x02000000u ? uint32_t(rva) : 0u;
}

void CountRuntimeModelCreateCallerFamily(uint32_t callerRva) {
  if (callerRva >= kBuildChildRuntimeModelLinksRva &&
      callerRva < kBuildChildRuntimeModelLinksEndRva) {
    g_runtimeModelCreateCallerBuildChildLinksCount.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }

  if (callerRva >= kCreateSpriteRuntimeRva &&
      callerRva < kCreateSpriteRuntimeCallerEndRva) {
    g_runtimeModelCreateCallerCreateSpriteRuntimeCount.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }

  g_runtimeModelCreateCallerOtherCount.fetch_add(1u,
                                                 std::memory_order_relaxed);
}

void CountRuntimeModelCtorCallerFamily(uint32_t callerRva) {
  if (callerRva >= kPromoteRuntimeModelRva &&
      callerRva < kPromoteRuntimeModelEndRva) {
    g_runtimeModelCtorCallerPromoteCount.fetch_add(1u,
                                                   std::memory_order_relaxed);
    return;
  }

  g_runtimeModelCtorCallerOtherCount.fetch_add(1u,
                                               std::memory_order_relaxed);
}

bool LooksLikeRuntimeModelPtr(void* candidate);

template <typename T>
bool TryReadTrustedValue(const void* base, size_t offset, T& out) {
  if (base == nullptr)
    return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  const uintptr_t addr = start + offset;
  if (addr < 0x10000u || addr < start)
    return false;
  out = *reinterpret_cast<const T*>(addr);
  return true;
}

void* TryReadTrustedPtr(const void* base, size_t offset) {
  void* value = nullptr;
  if (!TryReadTrustedValue(base, offset, value))
    return nullptr;
  return value;
}

uint32_t TryReadTrustedU32(const void* base, size_t offset) {
  uint32_t value = 0u;
  TryReadTrustedValue(base, offset, value);
  return value;
}

bool LooksLikeRuntimeModelPtrCached(void* candidate) {
  if (candidate == nullptr)
    return false;

  const uintptr_t candidateValue = reinterpret_cast<uintptr_t>(candidate);
  if (candidateValue < 0x10000u)
    return false;

  static thread_local std::unordered_set<void*> s_validRuntimeModels;
  if (s_validRuntimeModels.find(candidate) != s_validRuntimeModels.end())
    return true;

  if (!LooksLikeRuntimeModelPtr(candidate))
    return false;

  if (s_validRuntimeModels.size() < 8192u)
    s_validRuntimeModels.insert(candidate);
  return true;
}

void RecordRuntimePaletteTree(int runtimeModel,
                              const ModelInstanceRecord* ownerHint);
void RecordRuntimePaletteTreeIfStale(int runtimeModel,
                                     const ModelInstanceRecord* ownerHint);
bool RecordRuntimeMatrixPalette(int runtimeModel,
                                bool allowResourceBinding = true,
                                uint32_t* outMatrixCount = nullptr,
                                uint64_t* outMatrixHash = nullptr);
bool RecordRuntimeMatrixPaletteFromRangeCopy(int runtimeModel, int contextPtr,
                                             int sourceMatrixBasePtr,
                                             bool allowResourceBinding = true,
                                             bool publishPalette = true);
void* TryReadPtrFast(const void* base, size_t offset);
uint32_t TryReadU32Fast(const void* base, size_t offset);
void MergeAttachmentIdentityFromInstance(ModelInstanceRecord& dst,
                                         const ModelInstanceRecord& src);
void MergeAttachmentIdentityFromShadow(
    ModelInstanceRecord& dst, const render::ShadowObjectRecord& src);
void MergeAttachmentIdentityFromRender(
    ModelInstanceRecord& dst,
    const render::RenderObjectIdentitySnapshot& src);
bool HasAttachmentIdentity(const ModelInstanceRecord& record);
bool HasResolvedRenderIdentity(
    const render::RenderObjectIdentitySnapshot& snapshot);
bool TryResolveParentSpriteOwnerHint(void* spritePtr,
                                     void* currentRuntimeModelPtr,
                                     ModelInstanceRecord& out);
bool TryResolveIdentityFromPointerCandidate(void* candidatePtr,
                                            void* preferredRuntimeModelPtr,
                                            ModelInstanceRecord& out);
bool TryResolveAttachmentIdentityFromBlock(const void* blockPtr,
                                          uint32_t scanLimit,
                                          void* preferredRuntimeModelPtr,
                                          ModelInstanceRecord& out,
                                          uint32_t& outOffset,
                                          void*& outCandidatePtr);
bool TryResolveSourceObjectIdentityHint(void* sourceObjectPtr,
                                        void* preferredRuntimeModelPtr,
                                        ModelInstanceRecord& out,
                                        uint32_t& outOffset,
                                        void*& outCandidatePtr);
bool TryResolveSourceObjectIdentityHintDeep(void* sourceObjectPtr,
                                           void* preferredRuntimeModelPtr,
                                           ModelInstanceRecord& out,
                                           uint32_t& outOffset,
                                           void*& outCandidatePtr);
void RecordSpriteFrameSourceObjectFieldProbe(void* sourceObjectPtr,
                                             void* preferredRuntimeModelPtr);
bool TryResolveCurrentRenderOwnerHint(void* spritePtr, void* runtimeModelPtr,
                                      ModelInstanceRecord& out,
                                      render::ObjectKind& outKind);

bool TryResolveSourceObjectIdentity(void* sourceObjectPtr, void* spritePtr,
                                    void*& outUnitPtr, uint32_t& outJHandle,
                                    uint32_t& outRawcode,
                                    render::ObjectKind& outKind) {
  outUnitPtr = nullptr;
  outJHandle = 0u;
  outRawcode = 0u;
  outKind = render::ObjectKind::Unknown;
  if (sourceObjectPtr == nullptr)
    return false;

  auto acceptUnit = [&](void* unitPtr, uint32_t agentType) -> bool {
    if (unitPtr == nullptr)
      return false;

    game::UnitWrapper unit(unitPtr);
    if (!unit.IsValid())
      return false;

    const uint32_t rawcode = unit.GetRawcode();
    if (rawcode == 0u || !dxvk::war3::IsLikelyFourCC(rawcode))
      return false;

    const uint32_t jHandle = unit.GetJassHandle();
    void* sourceSpritePtr = unit.GetSprite();
    if (jHandle == 0u && sourceSpritePtr == nullptr && spritePtr == nullptr)
      return false;

    outUnitPtr = unitPtr;
    outJHandle = jHandle;
    outRawcode = rawcode;
    outKind = unit.GetKind(agentType);
    return true;
  };

  game::AgentWrapper agent(sourceObjectPtr);
  const uint32_t agentType =
      agent.IsValid() ? agent.GetTypeFourCC() : 0u;

  if (acceptUnit(sourceObjectPtr, agentType))
    return true;
  if (acceptUnit(agent.GetUnitPtr(), agentType))
    return true;

  const uint32_t widgetRawcode =
      TryReadU32Fast(sourceObjectPtr, dxvk::war3::CWidgetOffsets::TypeId);
  if (widgetRawcode != 0u && dxvk::war3::IsLikelyFourCC(widgetRawcode)) {
    outRawcode = widgetRawcode;
    if (agentType == AgentTypeFourCC::Destructible_LE ||
        agentType == AgentTypeFourCC::DestructibleID) {
      outKind = render::ObjectKind::Destructible;
    } else if (agentType == AgentTypeFourCC::Item_LE) {
      outKind = render::ObjectKind::Item;
    }
  }

  render::ShadowObjectRecord shadowRecord = {};
  if (render::ShadowObjectRegistry::instance().findByWorldObjectEntry(
          sourceObjectPtr, shadowRecord)) {
    g_sourceObjectRenderBridgeResolvedByEntryCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastSourceObjectRenderBridgeSourceObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectRenderBridgeSceneNodePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(shadowRecord.sceneNode)),
        std::memory_order_relaxed);
    if (outUnitPtr == nullptr)
      outUnitPtr = shadowRecord.unitPtr;
    if (outJHandle == 0u)
      outJHandle = shadowRecord.jHandle;
    if (outRawcode == 0u)
      outRawcode = shadowRecord.rawcode;
    if (outKind == render::ObjectKind::Unknown &&
        shadowRecord.kind != render::ObjectKind::Unknown) {
      outKind = shadowRecord.kind;
    }
  } else {
    void* sourceSceneNode = TryReadPtrFast(sourceObjectPtr, 0x20);
    if (sourceSceneNode != nullptr &&
        render::ShadowObjectRegistry::instance().findBySceneNode(
            sourceSceneNode, shadowRecord)) {
      g_sourceObjectRenderBridgeResolvedBySceneNodeCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSourceObjectRenderBridgeSourceObjectPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
          std::memory_order_relaxed);
      g_lastSourceObjectRenderBridgeSceneNodePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceSceneNode)),
          std::memory_order_relaxed);
      if (outUnitPtr == nullptr)
        outUnitPtr = shadowRecord.unitPtr;
      if (outJHandle == 0u)
        outJHandle = shadowRecord.jHandle;
      if (outRawcode == 0u)
        outRawcode = shadowRecord.rawcode;
      if (outKind == render::ObjectKind::Unknown &&
          shadowRecord.kind != render::ObjectKind::Unknown) {
        outKind = shadowRecord.kind;
      }
    }
  }

  void* preferredRuntimeModelPtr = nullptr;
  if (spritePtr != nullptr) {
    preferredRuntimeModelPtr =
        TryReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model);
    if (!LooksLikeRuntimeModelPtr(preferredRuntimeModelPtr))
      preferredRuntimeModelPtr = nullptr;
  }

  ModelInstanceRecord hintRecord = {};
  uint32_t hintOffset = 0u;
  void* hintCandidatePtr = nullptr;
  if (TryResolveSourceObjectIdentityHint(sourceObjectPtr, preferredRuntimeModelPtr,
                                         hintRecord, hintOffset,
                                         hintCandidatePtr)) {
    g_sourceObjectIdentityHintResolvedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintSourceObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintCandidatePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(hintCandidatePtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintOffset.store(hintOffset,
                                               std::memory_order_relaxed);
    if (outUnitPtr == nullptr)
      outUnitPtr = hintRecord.unitPtr;
    if (outJHandle == 0u)
      outJHandle = hintRecord.jHandle;
    if (outRawcode == 0u)
      outRawcode = hintRecord.rawcode;
  }

  return outUnitPtr != nullptr || outJHandle != 0u || outRawcode != 0u;
}

bool HasResolvedRenderIdentity(
    const render::RenderObjectIdentitySnapshot& snapshot) {
  return snapshot.worldObjectEntry != nullptr ||
         snapshot.unitPtr != nullptr || snapshot.jHandle != 0u ||
         snapshot.rawcode != 0u ||
         snapshot.kind != render::ObjectKind::Unknown;
}

bool HasUsableAttachmentRenderIdentity(
    const render::RenderObjectIdentitySnapshot& snapshot) {
  if (!HasResolvedRenderIdentity(snapshot))
    return false;

  if (snapshot.unitPtr != nullptr || snapshot.jHandle != 0u ||
      snapshot.rawcode != 0u) {
    return true;
  }

  const uintptr_t sceneNodeValue =
      reinterpret_cast<uintptr_t>(snapshot.sceneNode);
  const uintptr_t worldObjectEntryValue =
      reinterpret_cast<uintptr_t>(snapshot.worldObjectEntry);
  return snapshot.sceneNode != nullptr && sceneNodeValue >= 0x10000u &&
         snapshot.worldObjectEntry != nullptr &&
         worldObjectEntryValue >= 0x10000u;
}

bool TryResolveSourceObjectRenderIdentity(
    void* sourceObjectPtr, render::RenderObjectIdentitySnapshot& out) {
  out = {};
  if (sourceObjectPtr == nullptr)
    return false;

  render::ShadowObjectRecord shadowRecord = {};
  game::AgentWrapper sourceAgent(sourceObjectPtr);
  void* sourceUnitPtr = sourceObjectPtr;
  if (!game::UnitWrapper(sourceUnitPtr).IsValid())
    sourceUnitPtr = sourceAgent.GetUnitPtr();
  if (sourceUnitPtr != nullptr &&
      render::ShadowObjectRegistry::instance().findByUnitPtr(sourceUnitPtr,
                                                             shadowRecord)) {
    out.worldObjectEntry = shadowRecord.worldObjectEntry;
    out.sceneNode = shadowRecord.sceneNode;
    out.unitPtr = sourceUnitPtr;
    out.jHandle = shadowRecord.jHandle;
    out.rawcode = shadowRecord.rawcode;
    out.kind = shadowRecord.kind;
    return out.HasStableIdentity();
  }

  if (render::ShadowObjectRegistry::instance().findByWorldObjectEntry(
          sourceObjectPtr, shadowRecord)) {
    g_sourceObjectRenderBridgeResolvedByEntryCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastSourceObjectRenderBridgeSourceObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectRenderBridgeSceneNodePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(shadowRecord.sceneNode)),
        std::memory_order_relaxed);
    out.worldObjectEntry = sourceObjectPtr;
    out.sceneNode = shadowRecord.sceneNode;
    out.unitPtr = shadowRecord.unitPtr;
    out.jHandle = shadowRecord.jHandle;
    out.rawcode = shadowRecord.rawcode;
    out.kind = shadowRecord.kind;
    return out.HasStableIdentity();
  }

  void* sourceSceneNode = TryReadPtrFast(sourceObjectPtr, 0x20);
  if (sourceSceneNode != nullptr) {
    if (render::TryResolveRenderObjectIdentity(nullptr, sourceSceneNode, out) &&
        HasResolvedRenderIdentity(out)) {
      g_sourceObjectRenderBridgeResolvedBySceneNodeCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSourceObjectRenderBridgeSourceObjectPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
          std::memory_order_relaxed);
      g_lastSourceObjectRenderBridgeSceneNodePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceSceneNode)),
          std::memory_order_relaxed);
      if (out.sceneNode == nullptr)
        out.sceneNode = sourceSceneNode;
      return true;
    }

    if (render::ShadowObjectRegistry::instance().findBySceneNode(
            sourceSceneNode, shadowRecord)) {
      g_sourceObjectRenderBridgeResolvedBySceneNodeCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSourceObjectRenderBridgeSourceObjectPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
          std::memory_order_relaxed);
      g_lastSourceObjectRenderBridgeSceneNodePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceSceneNode)),
          std::memory_order_relaxed);
      out.sceneNode = sourceSceneNode;
      out.worldObjectEntry = shadowRecord.worldObjectEntry;
      out.unitPtr = shadowRecord.unitPtr;
      out.jHandle = shadowRecord.jHandle;
      out.rawcode = shadowRecord.rawcode;
      out.kind = shadowRecord.kind;
      if (out.HasStableIdentity())
        return true;
    }
  }

  ModelInstanceRecord hintRecord = {};
  uint32_t hintOffset = 0u;
  void* hintCandidatePtr = nullptr;
  if (TryResolveSourceObjectIdentityHint(sourceObjectPtr, nullptr, hintRecord,
                                         hintOffset, hintCandidatePtr)) {
    g_sourceObjectIdentityHintResolvedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintSourceObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintCandidatePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(hintCandidatePtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintOffset.store(hintOffset,
                                               std::memory_order_relaxed);
    out.worldObjectEntry = hintRecord.worldObjectEntry;
    out.sceneNode = hintRecord.sceneNode;
    out.unitPtr = hintRecord.unitPtr;
    out.jHandle = hintRecord.jHandle;
    out.rawcode = hintRecord.rawcode;
    return HasResolvedRenderIdentity(out);
  }

  return false;
}

void* TryReadSourceSpriteObjectPtr(void* sourceObjectPtr) {
  if (sourceObjectPtr == nullptr)
    return nullptr;

  void* sourceSpriteObjectPtr = TryReadPtrFast(sourceObjectPtr, 0x28);
  if (sourceSpriteObjectPtr == sourceObjectPtr)
    return nullptr;
  return sourceSpriteObjectPtr;
}

void PublishRuntimeSourceObject(void* runtimeModelPtr, void* spritePtr,
                                 void* sourceObjectPtr,
                                 void* sourceSpriteObjectPtr) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;
  if (sourceObjectPtr == nullptr && sourceSpriteObjectPtr == nullptr)
    return;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  instanceRegistry.noteRuntimeSourceObject(runtimeModelPtr, sourceObjectPtr,
                                           sourceSpriteObjectPtr, spritePtr);

  ModelInstanceRecord hintRecord = {};
  uint32_t hintOffset = 0u;
  void* hintCandidatePtr = nullptr;
  const bool hasSourceObjectHint = sourceObjectPtr != nullptr &&
      TryResolveSourceObjectIdentityHint(sourceObjectPtr, runtimeModelPtr,
                                         hintRecord, hintOffset,
                                         hintCandidatePtr);
  if (hasSourceObjectHint) {
    g_sourceObjectIdentityHintResolvedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintSourceObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintCandidatePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(hintCandidatePtr)),
        std::memory_order_relaxed);
    g_lastSourceObjectIdentityHintOffset.store(
        hintOffset, std::memory_order_relaxed);

    const void* hintedSourceObjectPtr =
        hintRecord.sourceObjectPtr != nullptr ? hintRecord.sourceObjectPtr
                                              : sourceObjectPtr;
    const void* hintedSourceSpriteObjectPtr =
        hintRecord.sourceSpriteObjectPtr != nullptr
            ? hintRecord.sourceSpriteObjectPtr
            : sourceSpriteObjectPtr;
    const void* hintedSpritePtr =
        hintRecord.spritePtr != nullptr ? hintRecord.spritePtr : spritePtr;
    instanceRegistry.noteRuntimeSourceObject(
        runtimeModelPtr, const_cast<void*>(hintedSourceObjectPtr),
        const_cast<void*>(hintedSourceSpriteObjectPtr),
        const_cast<void*>(hintedSpritePtr));
  }

  // When the upstream producer already gives us a source object, publish the
  // cheap owner hint immediately instead of waiting for a later render-side
  // bridge. This lets attachment rigid records inherit owner identity from the
  // same runtime family that published the source object.
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  render::ObjectKind kind = render::ObjectKind::Unknown;
  render::RenderObjectIdentitySnapshot sourceIdentity = {};
  const bool hasRenderIdentity =
      TryResolveSourceObjectRenderIdentity(sourceObjectPtr, sourceIdentity);
  if (hasRenderIdentity) {
    worldObjectEntry = sourceIdentity.worldObjectEntry;
    sceneNode = sourceIdentity.sceneNode;
    unitPtr = sourceIdentity.unitPtr;
    jHandle = sourceIdentity.jHandle;
    rawcode = sourceIdentity.rawcode;
    if (sourceIdentity.kind != render::ObjectKind::Unknown)
      kind = sourceIdentity.kind;
  }
  const bool hasSourceIdentity = TryResolveSourceObjectIdentity(
      sourceObjectPtr, spritePtr, unitPtr, jHandle, rawcode, kind);
  if (hasSourceObjectHint) {
    if (worldObjectEntry == nullptr)
      worldObjectEntry = hintRecord.worldObjectEntry;
    if (sceneNode == nullptr)
      sceneNode = hintRecord.sceneNode;
    if (unitPtr == nullptr)
      unitPtr = hintRecord.unitPtr;
    if (jHandle == 0u)
      jHandle = hintRecord.jHandle;
    if (rawcode == 0u)
      rawcode = hintRecord.rawcode;
  }
  if (hasRenderIdentity || hasSourceIdentity || HasAttachmentIdentity(hintRecord)) {
    if (worldObjectEntry != nullptr || sceneNode != nullptr ||
        unitPtr != nullptr || jHandle != 0u || rawcode != 0u) {
      instanceRegistry.noteInstanceIdentity(
          worldObjectEntry, sceneNode, unitPtr,
          hintRecord.spritePtr != nullptr ? hintRecord.spritePtr : spritePtr,
          jHandle, rawcode);
    }
    instanceRegistry.noteRuntimeOwnerIdentity(
        runtimeModelPtr, worldObjectEntry, sceneNode, unitPtr, spritePtr,
        jHandle, rawcode);
  }

  g_runtimeSourceObjectPublishCount.fetch_add(1u,
                                              std::memory_order_relaxed);
  g_lastRuntimeSourceObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeSourceSpriteObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceSpriteObjectPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeSourceRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
}

void PublishRuntimeSourceObjectLink(void* runtimeModelPtr, void* spritePtr,
                                    void* sourceObjectPtr,
                                    void* sourceSpriteObjectPtr) {
  if (!LooksLikeRuntimeModelPtrCached(runtimeModelPtr))
    return;
  if (sourceObjectPtr == nullptr && sourceSpriteObjectPtr == nullptr)
    return;

  ModelInstanceRegistry::instance().noteRuntimeSourceObject(
      runtimeModelPtr, sourceObjectPtr, sourceSpriteObjectPtr, spritePtr);
  g_runtimeSourceObjectPublishCount.fetch_add(1u,
                                              std::memory_order_relaxed);
  g_lastRuntimeSourceObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeSourceSpriteObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceSpriteObjectPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeSourceRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
}

void BootstrapRuntimeModelResourceLineage(void* runtimeModelPtr) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  void* modelResourcePtr =
      NormalizeDirectModelResourcePtr(
          TryResolveDirectModelResourceFromRuntimeModel(runtimeModelPtr));
  auto& resourceCache = ShadowModelResourceCache::instance();
  resourceCache.noteRuntimeModelBinding(runtimeModelPtr, modelResourcePtr, 0u);
  if (modelResourcePtr != nullptr)
    resourceCache.noteModelResourceBinding(modelResourcePtr, 0u);
}

void PublishBuildTimeChildRuntimeModelData(void* childRuntimeModelPtr,
                                           void* childModelDataPtr) {
  if (!LooksLikeRuntimeModelPtr(childRuntimeModelPtr) ||
      childModelDataPtr == nullptr ||
      g_buildChildRuntimeScopeState.depth == 0u) {
    return;
  }

  void* parentRuntimeModelPtr =
      g_buildChildRuntimeScopeState.parentRuntimeModelPtr;
  void* parentModelDataPtr = g_buildChildRuntimeScopeState.parentModelDataPtr;
  if (!LooksLikeRuntimeModelPtr(parentRuntimeModelPtr) ||
      parentModelDataPtr == nullptr) {
    return;
  }

  void* childModelResourcePtr =
      NormalizeDirectModelResourcePtr(childModelDataPtr);
  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto& resourceCache = ShadowModelResourceCache::instance();
  instanceRegistry.noteRuntimeCreationProvenance(
      childRuntimeModelPtr, childModelDataPtr,
      uint32_t(kBuildChildRuntimeModelLinksRva));
  resourceCache.noteRuntimeModelBinding(childRuntimeModelPtr,
                                        childModelResourcePtr, 0u);
  if (childModelResourcePtr != nullptr) {
    resourceCache.noteModelResourceBinding(childModelResourcePtr, 0u);
    g_runtimeChildBuildTimeDirectPublishWithResourceCount.fetch_add(
        1u, std::memory_order_relaxed);
  }

  void* parentModelResourcePtr =
      NormalizeDirectModelResourcePtr(parentModelDataPtr);
  resourceCache.noteRuntimeModelBinding(parentRuntimeModelPtr,
                                        parentModelResourcePtr, 0u);
  if (parentModelResourcePtr != nullptr)
    resourceCache.noteModelResourceBinding(parentModelResourcePtr, 0u);

  g_runtimeChildBuildTimeDirectPublishCount.fetch_add(
      1u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectParentModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentModelDataPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childModelDataPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectModelResourcePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childModelResourcePtr)),
      std::memory_order_relaxed);
}

void RecordRuntimeModelCreate(void* runtimeModelPtr, void* modelDataPtr,
                              uintptr_t callerPc) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  const uint32_t callerRva = GetModuleRvaFromAddress(callerPc);
  ModelInstanceRegistry::instance().noteRuntimeCreationProvenance(
      runtimeModelPtr, modelDataPtr, callerRva);
  g_runtimeModelCreateCount.fetch_add(1u, std::memory_order_relaxed);
  CountRuntimeModelCreateCallerFamily(callerRva);
  g_lastRuntimeModelCreateRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeModelCreateModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(modelDataPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeModelCreateCallerRva.store(callerRva,
                                          std::memory_order_relaxed);
}

void RecordRuntimeModelResolve(void* runtimeModelPtr, void* creatorHandlePtr,
                               uintptr_t callerPc) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  const uint32_t callerRva = GetModuleRvaFromAddress(callerPc);
  ModelInstanceRegistry::instance().noteRuntimeResolveProvenance(
      runtimeModelPtr, creatorHandlePtr, callerRva);
  g_runtimeModelResolveCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastRuntimeModelResolveRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeModelResolveHandlePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(creatorHandlePtr)),
      std::memory_order_relaxed);
  g_lastRuntimeModelResolveCallerRva.store(callerRva,
                                           std::memory_order_relaxed);
}

void RecordRuntimeModelCtor(void* runtimeModelPtr, uintptr_t callerPc,
                            bool isComplexCtor) {
  if (runtimeModelPtr == nullptr)
    return;

  const uintptr_t runtimeModelValue =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (runtimeModelValue < 0x10000u)
    return;

  const uint32_t callerRva = GetModuleRvaFromAddress(callerPc);
  ModelInstanceRegistry::instance().noteRuntimeCreationProvenance(
      runtimeModelPtr, nullptr, callerRva);
  g_runtimeModelCtorCount.fetch_add(1u, std::memory_order_relaxed);
  if (isComplexCtor) {
    g_runtimeModelComplexCtorCount.fetch_add(1u, std::memory_order_relaxed);
  } else {
    g_runtimeModelPlainCtorCount.fetch_add(1u, std::memory_order_relaxed);
  }
  CountRuntimeModelCtorCallerFamily(callerRva);
  g_lastRuntimeModelCtorRuntimeModelPtr.store(
      uint64_t(runtimeModelValue), std::memory_order_relaxed);
  g_lastRuntimeModelCtorCallerRva.store(callerRva,
                                        std::memory_order_relaxed);
  g_lastRuntimeModelCtorKind.store(isComplexCtor ? 2u : 1u,
                                   std::memory_order_relaxed);
}

void RecordRuntimeModelInitCopy(void* runtimeModelPtr, void* modelDataPtr,
                                uintptr_t callerPc) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  const uint32_t callerRva = GetModuleRvaFromAddress(callerPc);
  g_runtimeModelInitCopyCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastRuntimeModelInitRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeModelInitModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(modelDataPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeModelInitCallerRva.store(callerRva,
                                        std::memory_order_relaxed);

  ModelInstanceRecord existingRecord = {};
  const bool hasExistingCreate =
      ModelInstanceRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                           existingRecord) &&
      (existingRecord.runtimeCreatorCallerRva != 0u ||
       existingRecord.runtimeCreatorModelDataPtr != nullptr);
  if (hasExistingCreate)
    return;

  ModelInstanceRegistry::instance().noteRuntimeCreationProvenance(
      runtimeModelPtr, modelDataPtr, callerRva);
  g_runtimeModelInitCopyPublishedFallbackCount.fetch_add(
      1u, std::memory_order_relaxed);
}

void RecordSpriteHostOwnerBinding(void* hostPtr, void* sourceObjectPtr) {
  if (hostPtr == nullptr || sourceObjectPtr == nullptr)
    return;

  void* spritePtr = TryReadPtrFast(hostPtr, kSpriteHostBoundSpriteOffset);
  if (spritePtr == nullptr)
    return;

  void* runtimeModelPtr =
      TryReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model);
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  g_lastSpriteHostSourceObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
      std::memory_order_relaxed);
  g_lastSpriteHostSpritePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(spritePtr)),
      std::memory_order_relaxed);
  g_lastSpriteHostRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  PublishRuntimeSourceObject(runtimeModelPtr, spritePtr, sourceObjectPtr,
                             TryReadSourceSpriteObjectPtr(sourceObjectPtr));

  ModelInstanceRecord ownerRecord = {};
  ownerRecord.spritePtr = spritePtr;
  ownerRecord.runtimeModelPtr = runtimeModelPtr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  render::ObjectKind kind = render::ObjectKind::Unknown;
  if (TryResolveSourceObjectIdentity(sourceObjectPtr, spritePtr, unitPtr,
                                     jHandle, rawcode, kind)) {
    ownerRecord.unitPtr = unitPtr;
    ownerRecord.jHandle = jHandle;
    ownerRecord.rawcode = rawcode;
  }
  TryResolveCurrentRenderOwnerHint(spritePtr, runtimeModelPtr, ownerRecord,
                                   kind);
  if (!HasAttachmentIdentity(ownerRecord)) {
    return;
  }

  g_spriteHostBindResolvedIdentityCount.fetch_add(1u,
                                                  std::memory_order_relaxed);
  if (ownerRecord.unitPtr != nullptr)
    g_spriteHostBindResolvedUnitCount.fetch_add(1u,
                                                std::memory_order_relaxed);
  if (ownerRecord.jHandle != 0u)
    g_spriteHostBindResolvedHandleCount.fetch_add(1u,
                                                  std::memory_order_relaxed);
  if (ownerRecord.rawcode != 0u)
    g_spriteHostBindResolvedRawcodeCount.fetch_add(1u,
                                                   std::memory_order_relaxed);
  g_lastSpriteHostUnitPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.unitPtr)),
      std::memory_order_relaxed);
  g_lastSpriteHostJHandle.store(ownerRecord.jHandle, std::memory_order_relaxed);
  g_lastSpriteHostRawcode.store(ownerRecord.rawcode, std::memory_order_relaxed);

  render::NoteShadowRuntimeIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      spritePtr, ownerRecord.jHandle, ownerRecord.rawcode, kind);
  ModelInstanceRegistry::instance().noteRuntimeOwnerIdentity(
      runtimeModelPtr, ownerRecord.worldObjectEntry, ownerRecord.sceneNode,
      ownerRecord.unitPtr, spritePtr, ownerRecord.jHandle,
      ownerRecord.rawcode);

  if (!ModelInstanceRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                            ownerRecord)) {
    ownerRecord.runtimeModelPtr = runtimeModelPtr;
    ownerRecord.spritePtr = spritePtr;
  }
  RecordRuntimePaletteTree(int(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
                           &ownerRecord);
}

bool TryGetAttachedEffectRuntimeBinding(void* effectPtr,
                                        void*& outChildSpritePtr,
                                        void*& outRuntimeModelPtr) {
  outChildSpritePtr = nullptr;
  outRuntimeModelPtr = nullptr;
  if (effectPtr == nullptr)
    return false;

  outChildSpritePtr =
      TryReadPtrFast(effectPtr, dxvk::war3::CEffectOffsets::Sprite);
  if (outChildSpritePtr == nullptr)
    return false;

  outRuntimeModelPtr =
      TryReadPtrFast(outChildSpritePtr, dxvk::war3::CSpriteOffsets::Model);
  return LooksLikeRuntimeModelPtr(outRuntimeModelPtr);
}

void PublishAttachedEffectOwnerBinding(void* childSpritePtr,
                                       void* runtimeModelPtr,
                                       const ModelInstanceRecord& ownerRecord,
                                       render::ObjectKind kind) {
  if (childSpritePtr == nullptr || !LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  render::NoteShadowRuntimeIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      childSpritePtr, ownerRecord.jHandle, ownerRecord.rawcode, kind);
  render::NoteShadowRuntimeModelBinding(childSpritePtr, runtimeModelPtr,
                                        nullptr, std::string(), 0u, 0u, 0u);
  ModelInstanceRegistry::instance().noteRuntimeOwnerIdentity(
      runtimeModelPtr, ownerRecord.worldObjectEntry, ownerRecord.sceneNode,
      ownerRecord.unitPtr, childSpritePtr, ownerRecord.jHandle,
      ownerRecord.rawcode);

  RecordRuntimeMatrixPalette(int(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
                             false);
}

void RecordAttachedEffectInitOwnerBinding(void* effectPtr,
                                          void* ownerWidgetPtr) {
  if (effectPtr == nullptr || ownerWidgetPtr == nullptr)
    return;

  g_attachedEffectInitBindCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastAttachedEffectInitOwnerWidgetPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerWidgetPtr)),
      std::memory_order_relaxed);

  void* childSpritePtr = nullptr;
  void* runtimeModelPtr = nullptr;
  if (!TryGetAttachedEffectRuntimeBinding(effectPtr, childSpritePtr,
                                          runtimeModelPtr)) {
    return;
  }

  g_lastAttachedEffectInitChildSpritePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childSpritePtr)),
      std::memory_order_relaxed);
  g_lastAttachedEffectInitChildRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  PublishRuntimeSourceObject(runtimeModelPtr, childSpritePtr, ownerWidgetPtr,
                             TryReadSourceSpriteObjectPtr(ownerWidgetPtr));

  ModelInstanceRecord ownerRecord = {};
  ownerRecord.spritePtr = childSpritePtr;
  ownerRecord.runtimeModelPtr = runtimeModelPtr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  render::ObjectKind kind = render::ObjectKind::Unknown;
  if (TryResolveSourceObjectIdentity(ownerWidgetPtr, childSpritePtr, unitPtr,
                                     jHandle, rawcode, kind)) {
    ownerRecord.unitPtr = unitPtr;
    ownerRecord.jHandle = jHandle;
    ownerRecord.rawcode = rawcode;
  }
  TryResolveCurrentRenderOwnerHint(childSpritePtr, runtimeModelPtr, ownerRecord,
                                   kind);
  if (!HasAttachmentIdentity(ownerRecord)) {
    return;
  }

  g_attachedEffectInitResolvedIdentityCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (ownerRecord.unitPtr != nullptr)
    g_attachedEffectInitResolvedUnitCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (ownerRecord.jHandle != 0u)
    g_attachedEffectInitResolvedHandleCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (ownerRecord.rawcode != 0u)
    g_attachedEffectInitResolvedRawcodeCount.fetch_add(
        1u, std::memory_order_relaxed);
  g_lastAttachedEffectInitUnitPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.unitPtr)),
      std::memory_order_relaxed);
  g_lastAttachedEffectInitJHandle.store(ownerRecord.jHandle,
                                        std::memory_order_relaxed);
  g_lastAttachedEffectInitRawcode.store(ownerRecord.rawcode,
                                        std::memory_order_relaxed);

  PublishAttachedEffectOwnerBinding(childSpritePtr, runtimeModelPtr,
                                    ownerRecord, kind);
}

void RecordAttachedEffectDirectOwnerBinding(void* effectPtr,
                                            void* ownerWidgetPtr) {
  if (effectPtr == nullptr || ownerWidgetPtr == nullptr)
    return;

  g_attachedEffectDirectBindCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastAttachedEffectDirectOwnerWidgetPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerWidgetPtr)),
      std::memory_order_relaxed);

  void* childSpritePtr = nullptr;
  void* runtimeModelPtr = nullptr;
  if (!TryGetAttachedEffectRuntimeBinding(effectPtr, childSpritePtr,
                                          runtimeModelPtr)) {
    return;
  }

  g_lastAttachedEffectDirectChildSpritePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childSpritePtr)),
      std::memory_order_relaxed);
  g_lastAttachedEffectDirectChildRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  PublishRuntimeSourceObject(runtimeModelPtr, childSpritePtr, ownerWidgetPtr,
                             TryReadSourceSpriteObjectPtr(ownerWidgetPtr));

  ModelInstanceRecord ownerRecord = {};
  ownerRecord.spritePtr = childSpritePtr;
  ownerRecord.runtimeModelPtr = runtimeModelPtr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  render::ObjectKind kind = render::ObjectKind::Unknown;
  if (TryResolveSourceObjectIdentity(ownerWidgetPtr, childSpritePtr, unitPtr,
                                     jHandle, rawcode, kind)) {
    ownerRecord.unitPtr = unitPtr;
    ownerRecord.jHandle = jHandle;
    ownerRecord.rawcode = rawcode;
  }
  TryResolveCurrentRenderOwnerHint(childSpritePtr, runtimeModelPtr, ownerRecord,
                                   kind);
  if (!HasAttachmentIdentity(ownerRecord)) {
    return;
  }

  g_attachedEffectDirectResolvedIdentityCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (ownerRecord.unitPtr != nullptr)
    g_attachedEffectDirectResolvedUnitCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (ownerRecord.jHandle != 0u)
    g_attachedEffectDirectResolvedHandleCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (ownerRecord.rawcode != 0u)
    g_attachedEffectDirectResolvedRawcodeCount.fetch_add(
        1u, std::memory_order_relaxed);
  g_lastAttachedEffectDirectUnitPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.unitPtr)),
      std::memory_order_relaxed);
  g_lastAttachedEffectDirectJHandle.store(ownerRecord.jHandle,
                                          std::memory_order_relaxed);
  g_lastAttachedEffectDirectRawcode.store(ownerRecord.rawcode,
                                          std::memory_order_relaxed);

  PublishAttachedEffectOwnerBinding(childSpritePtr, runtimeModelPtr,
                                    ownerRecord, kind);
}

void MaybePublishAttachedEffectInitParentRuntimeOwnerIdentity(
    void* spritePtr, void* runtimeModelPtr) {
  if (g_attachedEffectInitScopeState.depth == 0u ||
      g_attachModelToPointScopeState.depth == 0u) {
    return;
  }

  void* parentSpritePtr = g_attachModelToPointScopeState.parentSpritePtr;
  void* parentRuntimeModelPtr =
      g_attachModelToPointScopeState.parentRuntimeModelPtr;
  if (spritePtr == nullptr || spritePtr != parentSpritePtr ||
      runtimeModelPtr == nullptr || runtimeModelPtr != parentRuntimeModelPtr ||
      !LooksLikeRuntimeModelPtr(parentRuntimeModelPtr)) {
    return;
  }

  void* ownerWidgetPtr = g_attachedEffectInitScopeState.ownerWidgetPtr;
  if (ownerWidgetPtr == nullptr)
    return;

  void* sourceSpriteObjectPtr = TryReadSourceSpriteObjectPtr(ownerWidgetPtr);
  PublishRuntimeSourceObject(parentRuntimeModelPtr, parentSpritePtr,
                             ownerWidgetPtr, sourceSpriteObjectPtr);

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  ModelInstanceRecord ownerRecord = {};
  ownerRecord.spritePtr = parentSpritePtr;
  ownerRecord.runtimeModelPtr = parentRuntimeModelPtr;
  ownerRecord.sourceObjectPtr = ownerWidgetPtr;
  ownerRecord.sourceSpriteObjectPtr = sourceSpriteObjectPtr;

  ModelInstanceRecord existingRecord = {};
  if (instanceRegistry.findByRuntimeModel(parentRuntimeModelPtr,
                                          existingRecord)) {
    MergeAttachmentIdentityFromInstance(ownerRecord, existingRecord);
  }
  if (instanceRegistry.findOwnerByRuntimeModel(parentRuntimeModelPtr,
                                               existingRecord)) {
    MergeAttachmentIdentityFromInstance(ownerRecord, existingRecord);
  }
  if (instanceRegistry.findBySpritePtr(parentSpritePtr, existingRecord)) {
    MergeAttachmentIdentityFromInstance(ownerRecord, existingRecord);
  }

  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  render::ObjectKind kind = render::ObjectKind::Unknown;
  if (TryResolveSourceObjectIdentity(ownerWidgetPtr, parentSpritePtr, unitPtr,
                                     jHandle, rawcode, kind)) {
    ownerRecord.unitPtr = unitPtr;
    ownerRecord.jHandle = jHandle;
    ownerRecord.rawcode = rawcode;
  }
  TryResolveCurrentRenderOwnerHint(parentSpritePtr, parentRuntimeModelPtr,
                                   ownerRecord, kind);
  if (!HasAttachmentIdentity(ownerRecord))
    return;

  g_attachedEffectInitParentRuntimeOwnerPublishCount.fetch_add(
      1u, std::memory_order_relaxed);
  g_lastAttachedEffectInitParentRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)),
      std::memory_order_relaxed);

  render::NoteShadowRuntimeIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      parentSpritePtr, ownerRecord.jHandle, ownerRecord.rawcode, kind);
  instanceRegistry.noteInstanceIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode,
      ownerRecord.unitPtr, parentSpritePtr, ownerRecord.jHandle,
      ownerRecord.rawcode);
  instanceRegistry.bindRuntimeModelToSprite(
      parentSpritePtr, parentRuntimeModelPtr, ownerRecord.modelKey,
      ownerRecord.modelResourcePtr);
  instanceRegistry.noteRuntimeOwnerIdentity(
      parentRuntimeModelPtr, ownerRecord.worldObjectEntry,
      ownerRecord.sceneNode, ownerRecord.unitPtr, parentSpritePtr,
      ownerRecord.jHandle, ownerRecord.rawcode);
  AttachmentRigidRegistry::instance().noteRuntimeIdentity(
      parentRuntimeModelPtr, ownerRecord.worldObjectEntry,
      ownerRecord.sceneNode, ownerRecord.unitPtr, ownerRecord.sourceObjectPtr,
      ownerRecord.sourceSpriteObjectPtr, ownerRecord.jHandle,
      ownerRecord.rawcode);
  RecordRuntimeMatrixPalette(
      int(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)), false);
}

bool TryResolveAttachModelToPointOwner(void* parentSpritePtr,
                                       void* childSpritePtr,
                                       void* childRuntimeModelPtr,
                                       ModelInstanceRecord& outOwnerRecord,
                                       render::ObjectKind& outKind) {
  outOwnerRecord = {};
  outKind = render::ObjectKind::Unknown;
  if (parentSpritePtr == nullptr)
    return false;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findBySpritePtr(parentSpritePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(outOwnerRecord, instanceRecord);

  void* parentRuntimeModelPtr =
      TryReadPtrFast(parentSpritePtr, dxvk::war3::CSpriteOffsets::Model);
  if (LooksLikeRuntimeModelPtr(parentRuntimeModelPtr)) {
    if (instanceRegistry.findOwnerByRuntimeModel(parentRuntimeModelPtr,
                                                 instanceRecord)) {
      MergeAttachmentIdentityFromInstance(outOwnerRecord, instanceRecord);
    }
    if (instanceRegistry.findByRuntimeModel(parentRuntimeModelPtr,
                                            instanceRecord)) {
      MergeAttachmentIdentityFromInstance(outOwnerRecord, instanceRecord);
    }
  }

  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findBySpritePtr(parentSpritePtr, shadowRecord)) {
    MergeAttachmentIdentityFromShadow(outOwnerRecord, shadowRecord);
    if (outKind == render::ObjectKind::Unknown &&
        shadowRecord.kind != render::ObjectKind::Unknown) {
      outKind = shadowRecord.kind;
    }
  }
  if (LooksLikeRuntimeModelPtr(parentRuntimeModelPtr) &&
      shadowRegistry.findByRuntimeModel(parentRuntimeModelPtr, shadowRecord)) {
    MergeAttachmentIdentityFromShadow(outOwnerRecord, shadowRecord);
    if (outKind == render::ObjectKind::Unknown &&
        shadowRecord.kind != render::ObjectKind::Unknown) {
      outKind = shadowRecord.kind;
    }
  }

  if (!HasAttachmentIdentity(outOwnerRecord) &&
      TryResolveParentSpriteOwnerHint(childSpritePtr, childRuntimeModelPtr,
                                      instanceRecord)) {
    MergeAttachmentIdentityFromInstance(outOwnerRecord, instanceRecord);
  }
  if (!HasAttachmentIdentity(outOwnerRecord) &&
      TryResolveParentSpriteOwnerHint(parentSpritePtr, childRuntimeModelPtr,
                                      instanceRecord)) {
    MergeAttachmentIdentityFromInstance(outOwnerRecord, instanceRecord);
  }

  if (!HasAttachmentIdentity(outOwnerRecord)) {
    if (outOwnerRecord.spritePtr == nullptr)
      outOwnerRecord.spritePtr = childSpritePtr;
    if (outOwnerRecord.runtimeModelPtr == nullptr)
      outOwnerRecord.runtimeModelPtr = childRuntimeModelPtr;
    TryResolveCurrentRenderOwnerHint(childSpritePtr, childRuntimeModelPtr,
                                     outOwnerRecord, outKind);
  }

  return HasAttachmentIdentity(outOwnerRecord);
}

void RecordAttachModelToPointOwnerBinding(void* parentSpritePtr,
                                          int attachPointIndex,
                                          void* childSpritePtr) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return;
  }
  if (!g_config.poseEnabled || !g_config.attachmentEnabled)
    return;
  if (parentSpritePtr == nullptr || childSpritePtr == nullptr)
    return;

  g_attachModelToPointBindCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastAttachModelToPointAttachPointIndex.store(
      uint32_t(attachPointIndex), std::memory_order_relaxed);
  g_lastAttachModelToPointParentSpritePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentSpritePtr)),
      std::memory_order_relaxed);
  g_lastAttachModelToPointChildSpritePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childSpritePtr)),
      std::memory_order_relaxed);

  void* childRuntimeModelPtr =
      TryReadPtrFast(childSpritePtr, dxvk::war3::CSpriteOffsets::Model);
  if (!LooksLikeRuntimeModelPtr(childRuntimeModelPtr))
    return;

  g_lastAttachModelToPointChildRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childRuntimeModelPtr)),
      std::memory_order_relaxed);

  ModelInstanceRecord ownerRecord = {};
  render::ObjectKind kind = render::ObjectKind::Unknown;
  if (!TryResolveAttachModelToPointOwner(parentSpritePtr, childSpritePtr,
                                         childRuntimeModelPtr, ownerRecord,
                                         kind)) {
    if (ownerRecord.sourceObjectPtr != nullptr ||
        ownerRecord.sourceSpriteObjectPtr != nullptr) {
      PublishRuntimeSourceObject(childRuntimeModelPtr, childSpritePtr,
                                 ownerRecord.sourceObjectPtr,
                                 ownerRecord.sourceSpriteObjectPtr);
    }
    return;
  }

  if (ownerRecord.sourceObjectPtr != nullptr ||
      ownerRecord.sourceSpriteObjectPtr != nullptr) {
    PublishRuntimeSourceObject(childRuntimeModelPtr, childSpritePtr,
                               ownerRecord.sourceObjectPtr,
                               ownerRecord.sourceSpriteObjectPtr);
  }

  g_attachModelToPointResolvedIdentityCount.fetch_add(1u,
                                                      std::memory_order_relaxed);
  if (ownerRecord.unitPtr != nullptr)
    g_attachModelToPointResolvedUnitCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (ownerRecord.jHandle != 0u)
    g_attachModelToPointResolvedHandleCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (ownerRecord.rawcode != 0u)
    g_attachModelToPointResolvedRawcodeCount.fetch_add(
        1u, std::memory_order_relaxed);
  g_lastAttachModelToPointUnitPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.unitPtr)),
      std::memory_order_relaxed);
  g_lastAttachModelToPointJHandle.store(ownerRecord.jHandle,
                                        std::memory_order_relaxed);
  g_lastAttachModelToPointRawcode.store(ownerRecord.rawcode,
                                        std::memory_order_relaxed);

  render::NoteShadowRuntimeIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      childSpritePtr, ownerRecord.jHandle, ownerRecord.rawcode, kind);
  render::NoteShadowRuntimeModelBinding(childSpritePtr, childRuntimeModelPtr,
                                        nullptr, std::string(), 0u, 0u, 0u);
  ModelInstanceRegistry::instance().bindRuntimeModelToSprite(
      childSpritePtr, childRuntimeModelPtr, ownerRecord.modelKey,
      ownerRecord.modelResourcePtr);
  ModelInstanceRegistry::instance().noteRuntimeOwnerIdentity(
      childRuntimeModelPtr, ownerRecord.worldObjectEntry,
      ownerRecord.sceneNode, ownerRecord.unitPtr, childSpritePtr,
      ownerRecord.jHandle, ownerRecord.rawcode);
  RecordRuntimeMatrixPalette(
      int(reinterpret_cast<uintptr_t>(childRuntimeModelPtr)), false);

  void* parentRuntimeModelPtr =
      TryReadPtrFast(parentSpritePtr, dxvk::war3::CSpriteOffsets::Model);
  if (LooksLikeRuntimeModelPtr(parentRuntimeModelPtr)) {
    AttachmentRigidRecord promotedRecord = {};
    void* previousChildRuntimeModelPtr = nullptr;
    if (AttachmentRigidRegistry::instance().promoteAttachmentChildRuntime(
            parentRuntimeModelPtr, childRuntimeModelPtr, childSpritePtr,
            &promotedRecord, &previousChildRuntimeModelPtr)) {
      g_attachModelToPointPromotedAttachmentChildRuntimeCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastAttachModelToPointPromotedOwnerRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(previousChildRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastAttachModelToPointPromotedChildRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(childRuntimeModelPtr)),
          std::memory_order_relaxed);

      ShadowModelResourceRecord childResource = {};
      void* childModelResourcePtr = nullptr;
      if (ShadowModelResourceCache::instance().findRuntimeModelResource(
              childRuntimeModelPtr, childResource)) {
        childModelResourcePtr = childResource.modelResourcePtr;
      }
      g_lastAttachModelToPointPromotedChildModelResourcePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(childModelResourcePtr)),
          std::memory_order_relaxed);
      if (childModelResourcePtr != nullptr) {
        g_attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount
            .fetch_add(1u, std::memory_order_relaxed);
      }
      AttachmentRigidRegistry::instance().noteRuntimeIdentity(
          childRuntimeModelPtr, promotedRecord.worldObjectEntry,
          promotedRecord.sceneNode, promotedRecord.unitPtr,
          promotedRecord.sourceObjectPtr, promotedRecord.sourceSpriteObjectPtr,
          promotedRecord.jHandle, promotedRecord.rawcode);
    }
  }
}

void MaybeLogBinding(void *spritePtr, void *runtimeModelPtr,
                     void *modelResourcePtr, uint64_t modelKey) {
  if (!g_config.logEnabled)
    return;

  const uint32_t count = g_bindLogCount.fetch_add(1, std::memory_order_relaxed);
  if (count < 32 || (count % 512u) == 0u) {
    war3dbg::Print(
        "DXVK_Model: bind sprite=%p runtime=%p model=%p key=0x%llX\n",
        spritePtr, runtimeModelPtr, modelResourcePtr,
        static_cast<unsigned long long>(modelKey));
  }
}

void MaybeLogPose(void *runtimeModelPtr, void *sceneNode, void *unitPtr,
                  float scale) {
  if (!g_config.logEnabled)
    return;

  const uint32_t count = g_poseLogCount.fetch_add(1, std::memory_order_relaxed);
  if (count < 16 || (count % 2048u) == 0u) {
    war3dbg::Print("DXVK_Model: pose runtime=%p scene=%p unit=%p scale=%.3f\n",
                   runtimeModelPtr, sceneNode, unitPtr, double(scale));
  }
}

void MaybeLogSpriteFrame(void* spritePtr, void* runtimeModelPtr, void* sceneNode,
                         void* unitPtr, float dt, uint32_t matrixCount) {
  if (!g_config.logEnabled)
    return;

  const uint32_t count =
      g_spriteFrameLogCount.fetch_add(1, std::memory_order_relaxed);
  if (count < 16 || (count % 2048u) == 0u) {
    war3dbg::Print(
        "DXVK_Model: spriteFrame sprite=%p runtime=%p scene=%p unit=%p dt=%.4f matrices=%u\n",
        spritePtr, runtimeModelPtr, sceneNode, unitPtr, double(dt),
        unsigned(matrixCount));
  }
}

Matrix4 DecodeRuntimePoseMatrix(const __m128i *poseMatrix) {
  if (poseMatrix == nullptr)
    return Matrix4();

  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseMatrix, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

Matrix4 DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {
  if (poseBytes == nullptr)
    return Matrix4();

  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

void* TryReadPtrFast(const void* base, size_t offset);
uint32_t TryReadU32Fast(const void* base, size_t offset);

uintptr_t ResolveGlobalBlendedPaletteBufferBase() {
  uintptr_t globalPaletteBuf =
      s_cachedGlobalPaletteBufBase.load(std::memory_order_acquire);
  if (globalPaletteBuf != 0u || g_gameBase == 0u)
    return globalPaletteBuf;

  void* bufPtr = nullptr;
  if (SafeReadPtrFast(reinterpret_cast<const void*>(g_gameBase + 0xBC6BD0u),
                      0u, bufPtr) &&
      bufPtr != nullptr) {
    globalPaletteBuf = reinterpret_cast<uintptr_t>(bufPtr);
    s_cachedGlobalPaletteBufBase.store(globalPaletteBuf,
                                       std::memory_order_release);
  }
  return globalPaletteBuf;
}

bool CaptureBlendedPaletteSlotRange(uint32_t startSlotIndex,
                                    const uint8_t* srcBase,
                                    uint32_t rawCount,
                                    uint32_t frameTag,
                                    std::atomic<uint64_t>* capturedCounter,
                                    std::atomic<uint64_t>* overflowCounter,
                                    std::atomic<uint64_t>* unreadableCounter) {
  if (srcBase == nullptr || rawCount == 0u ||
      startSlotIndex >= kSlotBlendedPaletteCacheSize)
    return false;

  const uint32_t capacityRemaining =
      kSlotBlendedPaletteCacheSize - startSlotIndex;
  const uint32_t capCount =
      std::min<uint32_t>(rawCount, kRuntimeMatrixBatchMaxCount);
  const uint32_t count = std::min<uint32_t>(capCount, capacityRemaining);
  if (count < rawCount && overflowCounter != nullptr)
    overflowCounter->fetch_add(1u, std::memory_order_relaxed);

  const size_t spanBytes = size_t(count) * 48u;
  if (count == 0u ||
      !dxvk::war3::IsReadableRangeFast(srcBase, spanBytes)) {
    if (count > 0u && unreadableCounter != nullptr)
      unreadableCounter->fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  const uint64_t baseSerial =
      s_slotBlendedPaletteWriteSerial.fetch_add(uint64_t(count),
                                                std::memory_order_relaxed) +
      1u;
  for (uint32_t i = 0u; i < count; ++i) {
    auto& entry = s_slotBlendedPaletteCache[startSlotIndex + i];
    entry.frameTag = frameTag;
    entry.writeSerial = baseSerial + uint64_t(i);
    entry.valid = true;
    entry.matrix = DecodeRuntimePoseMatrix48(srcBase + size_t(i) * 48u);
  }

  if (capturedCounter != nullptr)
    capturedCounter->fetch_add(uint64_t(count), std::memory_order_relaxed);
  return true;
}

static inline bool RenderablePartPaletteSnapshotEnabled() {
  static const bool enabled =
      GetEnvBoolCached("DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT", true);
  return enabled;
}

// Phase 7.47 dt gate probe - only mode。
// 打开时 Hook_SpriteFrameUpdate 等入口只记一笔 dt 分桶就 return，
// 不触发 RecordSpriteFramePoseFromSprite 等重路径；用于短时间诊断。
static inline bool SpriteUberDtProbeEnabled() {
  static const bool enabled =
      GetEnvBoolCached("DXVK_WAR3_SPRITE_UBER_DT_PROBE",
                        dxvk::war3::internal::
                            kWar3RuntimeConfigInstallSpriteUberDtProbeHooks);
  return enabled;
}

void RecordRenderablePartPaletteBinding(
    void* renderablePart,
    uint32_t paletteSlotIndex,
    uint32_t groupCount,
    uint32_t frameTag,
    RuntimeGroupPaletteProducerKind producerKind,
    const uint8_t* paletteBytes = nullptr,
    uint32_t paletteCount = 0u,
    void* runtimeModel = nullptr) {
  if (renderablePart == nullptr || paletteSlotIndex == 0xFFFFFFFFu ||
      paletteSlotIndex >= 0x3A98u)
    return;

  const uintptr_t partValue = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      (partValue >> 4u) % kRenderablePartPaletteBindingCacheSize;
  auto& entry = s_renderablePartPaletteBindings[slot];
  entry.paletteSlotIndex.store(paletteSlotIndex, std::memory_order_relaxed);
  entry.groupCount.store(groupCount, std::memory_order_relaxed);
  entry.frameTag.store(frameTag, std::memory_order_relaxed);
  entry.producerKind.store(static_cast<uint32_t>(producerKind),
                           std::memory_order_relaxed);

  if (RenderablePartPaletteSnapshotEnabled() && paletteBytes != nullptr &&
      paletteCount != 0u) {
    if (paletteCount > kRenderablePartPaletteSnapshotMaxCount) {
      entry.paletteWriteSerial.store(0u, std::memory_order_release);
      entry.paletteCount.store(0u, std::memory_order_relaxed);
      entry.paletteHash.store(0u, std::memory_order_relaxed);
      g_renderablePartPaletteSnapshotTooLargeCount.fetch_add(
          1u, std::memory_order_relaxed);
    } else if (!dxvk::war3::IsReadableRangeFast(
                   paletteBytes, size_t(paletteCount) * 48u)) {
      entry.paletteWriteSerial.store(0u, std::memory_order_release);
      entry.paletteCount.store(0u, std::memory_order_relaxed);
      entry.paletteHash.store(0u, std::memory_order_relaxed);
      g_renderablePartPaletteSnapshotUnreadableCount.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      const uint64_t snapshotSerial =
          s_renderablePartPaletteSnapshotSerial.fetch_add(
              1u, std::memory_order_relaxed) +
          1u;
      entry.paletteWriteSerial.store((snapshotSerial << 1u) | 1u,
                                     std::memory_order_release);
      for (uint32_t i = 0u; i < paletteCount; ++i) {
        entry.palette[i] =
            DecodeRuntimePoseMatrix48(paletteBytes + size_t(i) * 48u);
      }
      const uint64_t paletteHash =
          HashBytes(entry.palette.data(), size_t(paletteCount) * sizeof(Matrix4));
      entry.paletteCount.store(paletteCount, std::memory_order_relaxed);
      entry.paletteFrameTag.store(frameTag, std::memory_order_relaxed);
      entry.paletteHash.store(paletteHash, std::memory_order_relaxed);
      entry.paletteWriteSerial.store(snapshotSerial << 1u,
                                     std::memory_order_release);
      g_renderablePartPaletteSnapshotCapturedCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else {
    entry.paletteWriteSerial.store(0u, std::memory_order_release);
    entry.paletteCount.store(0u, std::memory_order_relaxed);
    entry.paletteHash.store(0u, std::memory_order_relaxed);
  }

  entry.writeSerial.store(
      s_renderablePartPaletteBindingSerial.fetch_add(
          1u, std::memory_order_relaxed) +
          1u,
      std::memory_order_release);
  // Phase 7.51：保存 producer 侧 runtimeModel，供 submit 时反查 PoseRegistry。
  entry.runtimeModel.store(
      reinterpret_cast<uintptr_t>(runtimeModel),
      std::memory_order_relaxed);
  entry.renderablePart.store(partValue, std::memory_order_release);
}

void CaptureRuntimeGroupPaletteBindings(
    int runtimeModel,
    RuntimeGroupPaletteProducerKind producerKind,
    bool captureSimpleFallbackSlots) {
  if (runtimeModel == 0 || !g_config.enabled)
    return;

  void* partArrayPtr = nullptr;
  uint32_t partCount = 0u;
  const void* runtimeModelPtr =
      reinterpret_cast<const void*>(uintptr_t(uint32_t(runtimeModel)));
  if (!SafeReadU32Fast(runtimeModelPtr, kCModelRenderablePartCountOffset,
                       partCount) ||
      partCount == 0u ||
      !SafeReadPtrFast(runtimeModelPtr, kCModelRenderablePartArrayOffset,
                       partArrayPtr) ||
      partArrayPtr == nullptr) {
    return;
  }

  partCount =
      std::min<uint32_t>(partCount, kMaxRuntimeRenderablePartsForPaletteScan);
  uint32_t frameTag = 0u;
  TryReadCurrentPaletteFrameTag(frameTag);

  const uintptr_t globalPaletteBuf = ResolveGlobalBlendedPaletteBufferBase();
  uint64_t partSeen = 0u;
  uint64_t bindingSeen = 0u;

  for (uint32_t i = 0u; i < partCount; ++i) {
    void* partPtr = nullptr;
    if (!SafeReadPtrFast(partArrayPtr, size_t(i) * sizeof(uint32_t), partPtr) ||
        partPtr == nullptr)
      continue;

    ++partSeen;
    const uint32_t skipFlag =
        TryReadU32Fast(partPtr, kRenderablePartSkipFlagOffset);
    if (skipFlag != 0u)
      continue;

    uint32_t slotIndex =
        TryReadU32Fast(partPtr, kRenderablePartPaletteSlotOffset);

    // Phase 7.52 根因修复：FROZEN 段里 War3 引擎的 8-帧 slot cadence 会让部分
    // renderablePart 的 +0x08 临时归为 0xFFFFFFFFu（slotIndex 未分配）。以前这里
    // 直接 continue，导致 s_renderablePartPaletteBindings 里这个 part 的 palette
    // snapshot 不再刷新，submit 端查到的永远是旧 bytes，形成"阴影动 0.5s 停
    // 0.5s"的视觉冻结。
    //
    // 实际情况：
    //   (1) `Hook_RuntimeMatrixWrite` (0x12E600) 每帧都在把最新的 blended palette
    //       写入全局 arena，按 slot 索引。
    //   (2) 同一个 renderablePart 在这 8 帧里通常属于同一个 CModel，它的逻辑
    //       slot 位置不会在 arena 里迁移。只是 `partPtr+0x08` 这个字段是由
    //       0x12FED0 每帧重新填写（或者这帧没填），和 writer 分开。
    //   (3) bindings 表里保留的上一次的 slotIndex 仍然是正确的 arena 位置。
    //
    // 因此 FROZEN 段里我们完全可以用 bindings 表上次记录的 slotIndex 去 arena 里
    // 重新取 fresh bytes，刷新 snapshot。视觉上等于"阴影每帧都跟着主模型走"。
    if (slotIndex == 0xFFFFFFFFu || slotIndex >= 0x3A98u) {
      const uintptr_t partValue = reinterpret_cast<uintptr_t>(partPtr);
      const size_t bindingSlot =
          (partValue >> 4u) % kRenderablePartPaletteBindingCacheSize;
      const auto& existingEntry = s_renderablePartPaletteBindings[bindingSlot];
      if (existingEntry.renderablePart.load(std::memory_order_acquire) ==
          partValue) {
        const uint32_t cachedSlotIndex =
            existingEntry.paletteSlotIndex.load(std::memory_order_relaxed);
        if (cachedSlotIndex != 0xFFFFFFFFu && cachedSlotIndex < 0x3A98u) {
          slotIndex = cachedSlotIndex;
        } else {
          continue;
        }
      } else {
        continue;
      }
    }

    uint32_t groupCount = 1u;
    if (!captureSimpleFallbackSlots) {
      if (void* geosetData =
              TryReadPtrFast(partPtr, kRenderablePartGeosetDataOffset)) {
        const uint32_t rawGroupCount =
            TryReadU32Fast(geosetData, kGeosetDataGroupCountOffset);
        if (rawGroupCount != 0u)
          groupCount = rawGroupCount;
      }
    }

    const uint8_t* matrixBytes = nullptr;
    if (globalPaletteBuf != 0u && groupCount != 0u &&
        slotIndex + groupCount <= 0x3A98u) {
      matrixBytes = reinterpret_cast<const uint8_t*>(
          globalPaletteBuf + size_t(slotIndex) * 48u);
    }

    RecordRenderablePartPaletteBinding(partPtr, slotIndex, groupCount, frameTag,
                                       producerKind, matrixBytes, groupCount,
                                       // Phase 7.51：传入 producer 侧 runtimeModel，
                                       // 让 renderablePart 反查能拿到正确的 PoseRegistry key。
                                       const_cast<void*>(runtimeModelPtr));
    ++bindingSeen;

    if (captureSimpleFallbackSlots && matrixBytes != nullptr) {
      CaptureBlendedPaletteSlotRange(
          slotIndex, matrixBytes, 1u, frameTag,
          &g_runtimeSimpleGroupPaletteSlotCapturedCount, nullptr,
          &g_runtimeSimpleGroupPaletteSlotUnreadableCount);
    }
  }

  if (producerKind == RuntimeGroupPaletteProducerKind::AllocAndFillWrapper) {
    g_runtimeGroupPaletteWrapperPartCount.fetch_add(
        partSeen, std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperBindingCount.fetch_add(
        bindingSeen, std::memory_order_relaxed);
  }
}

void *TryReadPtrFast(const void *base, size_t offset) {
  void *value = nullptr;
  if (!base)
    return nullptr;
  if (!SafeReadPtrFast(base, offset, value))
    return nullptr;
  return value;
}

uint32_t TryReadU32Fast(const void *base, size_t offset) {
  uint32_t value = 0;
  if (!base)
    return 0;
  SafeReadU32Fast(base, offset, value);
  return value;
}

float TryReadF32Fast(const void* base, size_t offset) {
  float value = 0.0f;
  if (!base)
    return 0.0f;
  SafeReadFast(base, offset, value);
  return value;
}

bool LooksLikeRuntimeModelPtr(void* candidate);

void* TryReadContextRuntimeModel(const void* contextPtr) {
  if (contextPtr == nullptr)
    return nullptr;
  void* runtimeModelPtr = TryReadPtrFast(contextPtr, 0u);
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return nullptr;
  return runtimeModelPtr;
}

void* TryReadContextRuntimeModelHot(const void* contextPtr) {
  if (contextPtr == nullptr)
    return nullptr;
  void* runtimeModelPtr = TryReadTrustedPtr(contextPtr, 0u);
  if (!LooksLikeRuntimeModelPtrCached(runtimeModelPtr))
    return nullptr;
  return runtimeModelPtr;
}

const dxvk::war3::analysis::GxStagePresetRecord*
TryReadRuntimeOverridePreset(void* runtimeModelPtr, uint32_t slotIndex,
                             size_t outputsOffset) {
  if (runtimeModelPtr == nullptr)
    return nullptr;

  const auto* outputBundle =
      reinterpret_cast<const uint8_t*>(runtimeModelPtr) +
      kRuntimeOverrideOutputBundleOffset;
  auto* presetBase = reinterpret_cast<
      dxvk::war3::analysis::GxStagePresetRecord*>(
      TryReadPtrFast(outputBundle, outputsOffset));
  if (presetBase == nullptr)
    return nullptr;

  auto* preset = presetBase + slotIndex;
  if (!IsReadableRange(preset, sizeof(*preset)))
    return nullptr;
  return preset;
}

const dxvk::war3::analysis::RenderOverrideLocalPointOutputRecord*
TryReadRuntimeLocalPointOutput(void* runtimeModelPtr, uint32_t slotIndex) {
  if (runtimeModelPtr == nullptr)
    return nullptr;

  auto* outputBase = reinterpret_cast<
      dxvk::war3::analysis::RenderOverrideLocalPointOutputRecord*>(
      TryReadPtrFast(runtimeModelPtr, kRuntimeLocalPointOutputArrayOffset));
  if (outputBase == nullptr)
    return nullptr;

  auto* output = outputBase + slotIndex;
  if (!IsReadableRange(output, sizeof(*output)))
    return nullptr;
  return output;
}

const dxvk::war3::analysis::RenderOverrideLocalPointOutputRecord*
TryReadRuntimeLocalPointOutputHot(void* runtimeModelPtr, uint32_t slotIndex) {
  if (runtimeModelPtr == nullptr || slotIndex > 8192u)
    return nullptr;

  auto* outputBase = reinterpret_cast<
      dxvk::war3::analysis::RenderOverrideLocalPointOutputRecord*>(
      TryReadTrustedPtr(runtimeModelPtr, kRuntimeLocalPointOutputArrayOffset));
  if (outputBase == nullptr)
    return nullptr;

  return outputBase + slotIndex;
}

uint32_t TryReadRuntimeMatrixCountFast(void* runtimeModelPtr) {
  if (runtimeModelPtr == nullptr)
    return 0u;
  return TryReadU32Fast(runtimeModelPtr,
                        dxvk::war3::CModelOffsets::FinalPoseMatrixCount);
}

bool LooksLikeWar3ForwardListNode(void* linkNode) {
  if (linkNode == nullptr)
    return false;
  return IsReadableRangeFast(linkNode, 16u);
}

bool HasReadableModelDataChildRuntimeLinkHead(void* modelDataPtr) {
  if (modelDataPtr == nullptr)
    return false;

  uint32_t childGroupCount = 0u;
  void* childGroupRecords = nullptr;
  if (!SafeReadU32Fast(modelDataPtr,
                       dxvk::war3::CModelDataOffsets::ChildRuntimeGroupCount,
                       childGroupCount) ||
      !SafeReadPtrFast(modelDataPtr,
                       dxvk::war3::CModelDataOffsets::ChildRuntimeGroupRecords,
                       childGroupRecords) ||
      childGroupCount == 0u || childGroupCount > 1024u ||
      childGroupRecords == nullptr ||
      !IsReadableRange(childGroupRecords, size_t(childGroupCount) * 12u)) {
    return false;
  }

  const auto* childGroups = reinterpret_cast<const uint8_t*>(childGroupRecords);
  for (uint32_t i = 0u; i < childGroupCount; ++i) {
    void* linkNode = nullptr;
    if (SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode) &&
        LooksLikeWar3ForwardListNode(linkNode)) {
      return true;
    }
  }
  return false;
}

struct ModelDataChildLinkScanProbe {
  uint32_t groupCount = 0u;
  uint32_t linkCount = 0u;
  uint32_t unreadableLinkCount = 0u;
  void* groupRecordsPtr = nullptr;
  void* firstHeadPtr = nullptr;
  void* firstLinkNodePtr = nullptr;
  void* firstChildModelDataPtr = nullptr;
  void* firstChildModelResourcePtr = nullptr;
  uint32_t firstSourceMeta = 0u;
};

void ScanModelDataChildRuntimeLinks(
    void* modelDataPtr, std::vector<ModelDataChildLinkProbeRecord>* out,
    ModelDataChildLinkScanProbe* probe, bool requireReadableNode) {
  if (out != nullptr)
    out->clear();
  if (probe != nullptr)
    *probe = ModelDataChildLinkScanProbe();
  if (modelDataPtr == nullptr)
    return;

  uint32_t childGroupCount = 0u;
  void* childGroupRecords = nullptr;
  if (!SafeReadU32Fast(modelDataPtr,
                       dxvk::war3::CModelDataOffsets::ChildRuntimeGroupCount,
                       childGroupCount) ||
      !SafeReadPtrFast(modelDataPtr,
                       dxvk::war3::CModelDataOffsets::ChildRuntimeGroupRecords,
                       childGroupRecords) ||
      childGroupCount == 0u || childGroupCount > 1024u ||
      childGroupRecords == nullptr ||
      !IsReadableRange(childGroupRecords, size_t(childGroupCount) * 12u)) {
    return;
  }

  if (probe != nullptr) {
    probe->groupCount = childGroupCount;
    probe->groupRecordsPtr = childGroupRecords;
  }

  std::unordered_set<void*> visitedLinkNodes;
  visitedLinkNodes.reserve(128u);
  const auto* childGroups = reinterpret_cast<const uint8_t*>(childGroupRecords);
  constexpr size_t kMaxLinkNodes = 1024u;
  for (uint32_t i = 0u; i < childGroupCount; ++i) {
    void* linkNode = nullptr;
    SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode);
    if (probe != nullptr && probe->firstHeadPtr == nullptr &&
        linkNode != nullptr) {
      probe->firstHeadPtr = linkNode;
    }

    std::vector<ModelDataChildLinkProbeRecord> bucketRecords;
    size_t traversed = 0u;
    while (linkNode != nullptr && traversed < kMaxLinkNodes &&
           visitedLinkNodes.insert(linkNode).second) {
      ++traversed;
      if (requireReadableNode && !LooksLikeWar3ForwardListNode(linkNode))
        break;

      ModelDataChildLinkProbeRecord record = {};
      record.linkNode = linkNode;
      record.bucketIndex = i;
      void* nextLinkNode = nullptr;
      const bool readOk =
          SafeReadPtrFast(linkNode, 8u, record.childModelDataPtr) &&
          SafeReadU32Fast(linkNode, 12u, record.sourceMeta) &&
          SafeReadPtrFast(linkNode, 4u, nextLinkNode);
      if (!readOk) {
        if (probe != nullptr)
          ++probe->unreadableLinkCount;
        break;
      }

      if (probe != nullptr) {
        ++probe->linkCount;
        if (probe->firstLinkNodePtr == nullptr) {
          probe->firstLinkNodePtr = linkNode;
          probe->firstChildModelDataPtr = record.childModelDataPtr;
          probe->firstChildModelResourcePtr =
              NormalizeDirectModelResourcePtr(record.childModelDataPtr);
          probe->firstSourceMeta = record.sourceMeta;
        }
      }
      bucketRecords.push_back(record);
      linkNode = nextLinkNode;
    }

    // BuildChildRuntimeModelLinks pushes runtime links at the bucket head, so
    // reverse resource-side order to align with post-build runtime traversal.
    std::reverse(bucketRecords.begin(), bucketRecords.end());
    if (out != nullptr)
      out->insert(out->end(), bucketRecords.begin(), bucketRecords.end());
  }
}

void CollectDirectChildRuntimeLinks(
    void* rootRuntimeModelPtr, std::vector<RuntimeChildLinkProbeRecord>& out) {
  out.clear();
  if (rootRuntimeModelPtr == nullptr)
    return;

  uint32_t childGroupCount = 0u;
  void* childGroupArray = nullptr;
  if (!SafeReadU32Fast(rootRuntimeModelPtr,
                       dxvk::war3::CModelOffsets::ChildBucketCount,
                       childGroupCount) ||
      !SafeReadPtrFast(rootRuntimeModelPtr,
                       dxvk::war3::CModelOffsets::ChildBucketArray,
                       childGroupArray) ||
      childGroupCount == 0u || childGroupArray == nullptr ||
      !IsReadableRange(childGroupArray, size_t(childGroupCount) * 12u)) {
    return;
  }

  std::unordered_set<void*> visitedLinkNodes;
  visitedLinkNodes.reserve(128u);
  const auto* childGroups = reinterpret_cast<const uint8_t*>(childGroupArray);
  constexpr size_t kMaxLinkNodes = 1024u;
  for (uint32_t i = 0u; i < childGroupCount; ++i) {
    void* linkNode = nullptr;
    SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode);
    size_t traversed = 0u;
    while (linkNode != nullptr && traversed < kMaxLinkNodes &&
           visitedLinkNodes.insert(linkNode).second) {
      ++traversed;
      RuntimeChildLinkProbeRecord record = {};
      record.linkNode = linkNode;
      record.bucketIndex = i;
      SafeReadPtrFast(linkNode, 8u, record.childRuntimeModelPtr);
      SafeReadU32Fast(linkNode, 12u, record.sourceMeta);
      record.tag = record.sourceMeta;
      out.push_back(record);

      void* nextLinkNode = nullptr;
      SafeReadPtrFast(linkNode, 4u, nextLinkNode);
      linkNode = nextLinkNode;
    }
  }
}

bool TryFindDirectChildRuntimeLinkByTag(void* rootRuntimeModelPtr,
                                        uint32_t slotIndex,
                                        uint32_t sourceRecordIndex,
                                        RuntimeChildLinkProbeRecord& out,
                                        uint32_t& outLinkCount,
                                        uint32_t& outMaxTag) {
  out = {};
  outLinkCount = 0u;
  outMaxTag = 0u;
  if (rootRuntimeModelPtr == nullptr)
    return false;

  uint32_t childGroupCount = 0u;
  void* childGroupArray = nullptr;
  if (!SafeReadU32Fast(rootRuntimeModelPtr,
                       dxvk::war3::CModelOffsets::ChildBucketCount,
                       childGroupCount) ||
      !SafeReadPtrFast(rootRuntimeModelPtr,
                       dxvk::war3::CModelOffsets::ChildBucketArray,
                       childGroupArray) ||
      childGroupCount == 0u || childGroupArray == nullptr ||
      !IsReadableRange(childGroupArray, size_t(childGroupCount) * 12u)) {
    return false;
  }

  RuntimeChildLinkProbeRecord sourceRecordMatch = {};
  const bool allowSourceRecordFallback =
      dxvk::war3::internal::kShadowAttachmentRigidAllowSourceRecordKeyFallback;
  const auto* childGroups = reinterpret_cast<const uint8_t*>(childGroupArray);
  constexpr size_t kMaxLinkNodesPerBucket = 256u;
  constexpr size_t kMaxTotalLinkNodes = 1024u;
  for (uint32_t i = 0u; i < childGroupCount && outLinkCount < kMaxTotalLinkNodes;
       ++i) {
    void* linkNode = nullptr;
    SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode);
    size_t traversed = 0u;
    while (linkNode != nullptr && traversed < kMaxLinkNodesPerBucket &&
           outLinkCount < kMaxTotalLinkNodes) {
      ++traversed;
      ++outLinkCount;

      RuntimeChildLinkProbeRecord record = {};
      record.linkNode = linkNode;
      record.bucketIndex = i;
      SafeReadPtrFast(linkNode, 8u, record.childRuntimeModelPtr);
      SafeReadU32Fast(linkNode, 12u, record.sourceMeta);
      record.tag = record.sourceMeta;
      outMaxTag = std::max(outMaxTag, record.tag);

      if (record.childRuntimeModelPtr != nullptr &&
          record.tag == slotIndex) {
        out = record;
        return true;
      }
      if (allowSourceRecordFallback &&
          sourceRecordMatch.childRuntimeModelPtr == nullptr &&
          record.childRuntimeModelPtr != nullptr &&
          record.tag == sourceRecordIndex) {
        sourceRecordMatch = record;
      }

      void* nextLinkNode = nullptr;
      SafeReadPtrFast(linkNode, 4u, nextLinkNode);
      if (nextLinkNode == linkNode)
        break;
      linkNode = nextLinkNode;
    }
  }

  if (sourceRecordMatch.childRuntimeModelPtr != nullptr) {
    out = sourceRecordMatch;
    return true;
  }
  return false;
}

void CollectModelDataChildRuntimeLinks(
    void* modelDataPtr, std::vector<ModelDataChildLinkProbeRecord>& out) {
  ScanModelDataChildRuntimeLinks(modelDataPtr, &out, nullptr,
                                 true /* requireReadableNode */);
}

void RecordBuildTimeModelDataChildLinkScan(
    void* parentRuntimeModelPtr, void* modelDataPtr, uint32_t phase,
    std::vector<ModelDataChildLinkProbeRecord>* outLinks = nullptr) {
  ModelDataChildLinkScanProbe probe = {};
  ScanModelDataChildRuntimeLinks(modelDataPtr, outLinks, &probe,
                                 false /* requireReadableNode */);

  if (phase == 1u) {
    g_runtimeChildBuildModelDataPreLinkCount.fetch_add(
        probe.linkCount, std::memory_order_relaxed);
    g_runtimeChildBuildModelDataPreUnreadableLinkCount.fetch_add(
        probe.unreadableLinkCount, std::memory_order_relaxed);
  } else if (phase == 2u) {
    g_runtimeChildBuildModelDataPostLinkCount.fetch_add(
        probe.linkCount, std::memory_order_relaxed);
    g_runtimeChildBuildModelDataPostUnreadableLinkCount.fetch_add(
        probe.unreadableLinkCount, std::memory_order_relaxed);
  }

  g_lastRuntimeChildBuildModelDataParentRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(modelDataPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataGroupRecordsPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(probe.groupRecordsPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataHeadPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(probe.firstHeadPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataLinkNodePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(probe.firstLinkNodePtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataChildModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(probe.firstChildModelDataPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataChildModelResourcePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(probe.firstChildModelResourcePtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataPhase.store(phase,
                                             std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataGroupCount.store(
      probe.groupCount, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataLinkCount.store(
      probe.linkCount, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataUnreadableLinkCount.store(
      probe.unreadableLinkCount, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataSourceMeta.store(
      probe.firstSourceMeta, std::memory_order_relaxed);
}

void BootstrapRuntimeChildLineageFromModelData(
    void* parentRuntimeModelPtr, void* modelDataPtr,
    const std::vector<RuntimeChildLinkProbeRecord>& childLinks,
    const std::vector<ModelDataChildLinkProbeRecord>* capturedModelDataLinks =
        nullptr) {
  if (!LooksLikeRuntimeModelPtr(parentRuntimeModelPtr) || modelDataPtr == nullptr)
    return;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto& resourceCache = ShadowModelResourceCache::instance();

  instanceRegistry.noteRuntimeCreationProvenance(
      parentRuntimeModelPtr, modelDataPtr, uint32_t(kBuildChildRuntimeModelLinksRva));
  void* parentModelResourcePtr = NormalizeDirectModelResourcePtr(modelDataPtr);
  resourceCache.noteRuntimeModelBinding(parentRuntimeModelPtr, parentModelResourcePtr,
                                        0u);
  if (parentModelResourcePtr != nullptr)
    resourceCache.noteModelResourceBinding(parentModelResourcePtr, 0u);

  if (childLinks.empty())
    return;

  std::vector<ModelDataChildLinkProbeRecord> modelDataChildLinks;
  if (capturedModelDataLinks != nullptr) {
    modelDataChildLinks = *capturedModelDataLinks;
  } else {
    CollectModelDataChildRuntimeLinks(modelDataPtr, modelDataChildLinks);
  }
  if (modelDataChildLinks.empty())
    return;

  std::array<std::vector<RuntimeChildLinkProbeRecord>, 64u> runtimeBuckets = {};
  std::array<std::vector<ModelDataChildLinkProbeRecord>, 64u> modelDataBuckets = {};
  for (const auto& childLink : childLinks) {
    if (!LooksLikeRuntimeModelPtr(childLink.childRuntimeModelPtr))
      continue;
    if (childLink.bucketIndex >= runtimeBuckets.size())
      continue;
    runtimeBuckets[childLink.bucketIndex].push_back(childLink);
  }

  for (const auto& childLink : modelDataChildLinks) {
    if (childLink.childModelDataPtr == nullptr)
      continue;
    if (childLink.bucketIndex >= modelDataBuckets.size())
      continue;
    modelDataBuckets[childLink.bucketIndex].push_back(childLink);
  }

  for (size_t bucketIndex = 0u; bucketIndex < runtimeBuckets.size();
       ++bucketIndex) {
    const auto& runtimeBucket = runtimeBuckets[bucketIndex];
    const auto& modelDataBucket = modelDataBuckets[bucketIndex];
    if (runtimeBucket.empty() || modelDataBucket.empty())
      continue;

    const size_t pairCount = std::min(runtimeBucket.size(), modelDataBucket.size());
    for (size_t i = 0u; i < pairCount; ++i) {
      const auto& runtimeChild = runtimeBucket[i];
      const auto& modelDataChild = modelDataBucket[i];
      if (runtimeChild.sourceMeta != 0u && modelDataChild.sourceMeta != 0u &&
          runtimeChild.sourceMeta != modelDataChild.sourceMeta) {
        continue;
      }

      instanceRegistry.noteRuntimeCreationProvenance(
          runtimeChild.childRuntimeModelPtr, modelDataChild.childModelDataPtr,
          uint32_t(kBuildChildRuntimeModelLinksRva));
      void* childModelResourcePtr =
          NormalizeDirectModelResourcePtr(modelDataChild.childModelDataPtr);
      resourceCache.noteRuntimeModelBinding(runtimeChild.childRuntimeModelPtr,
                                            childModelResourcePtr, 0u);
      if (childModelResourcePtr != nullptr)
        resourceCache.noteModelResourceBinding(childModelResourcePtr, 0u);
    }
  }
}

bool InternalTryBootstrapRuntimeChildLineageFromParentModelData(
    void* parentRuntimeModelPtr, void* parentModelDataPtr,
    void* childRuntimeModelPtr, uint32_t sourceMeta, uint32_t bucketIndex,
    void*& outChildModelDataPtr, void*& outChildModelResourcePtr,
    const std::vector<RuntimeChildLinkProbeRecord>* observedRuntimeLinks =
        nullptr) {
  outChildModelDataPtr = nullptr;
  outChildModelResourcePtr = nullptr;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return false;
  }
  if (!g_config.poseEnabled || !g_config.attachmentEnabled)
    return false;
  if (!LooksLikeRuntimeModelPtr(parentRuntimeModelPtr) ||
      !LooksLikeRuntimeModelPtr(childRuntimeModelPtr) ||
      parentModelDataPtr == nullptr || sourceMeta == 0u) {
    return false;
  }

  g_attachmentChildLineageBootstrapAttemptCount.fetch_add(
      1u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapParentRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapChildRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(childRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapParentModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentModelDataPtr)),
      std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapSourceMeta.store(
      sourceMeta, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapBucketIndex.store(
      bucketIndex, std::memory_order_relaxed);

  std::vector<ModelDataChildLinkProbeRecord> modelDataChildLinks;
  CollectModelDataChildRuntimeLinks(parentModelDataPtr, modelDataChildLinks);
  g_lastAttachmentChildLineageBootstrapModelDataLinkCount.store(
      uint32_t(std::min<size_t>(modelDataChildLinks.size(), 0xFFFFFFFFu)),
      std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapRuntimeLinkCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapStrictCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapSourceCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapBucketCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapAllCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapModelDataBucketCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  if (modelDataChildLinks.empty()) {
    g_attachmentChildLineageBootstrapMissNoModelDataLinksCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  auto countCandidates = [&](bool requireBucketMatch,
                             bool requireSourceMetaMatch) -> uint32_t {
    uint32_t count = 0u;
    for (const auto& childLink : modelDataChildLinks) {
      if (childLink.childModelDataPtr == nullptr)
        continue;
      if (requireBucketMatch && childLink.bucketIndex != bucketIndex)
        continue;
      if (requireSourceMetaMatch && childLink.sourceMeta != sourceMeta)
        continue;
      if (count < 2u) {
        void* modelResourcePtr =
            NormalizeDirectModelResourcePtr(childLink.childModelDataPtr);
        if (count == 0u) {
          g_lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(
                  childLink.childModelDataPtr)),
              std::memory_order_relaxed);
          g_lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(modelResourcePtr)),
              std::memory_order_relaxed);
        } else {
          g_lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(
                  childLink.childModelDataPtr)),
              std::memory_order_relaxed);
          g_lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(modelResourcePtr)),
              std::memory_order_relaxed);
        }
      }
      ++count;
    }
    return count;
  };
  g_lastAttachmentChildLineageBootstrapStrictCandidateCount.store(
      countCandidates(true, true), std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapSourceCandidateCount.store(
      countCandidates(false, true), std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapBucketCandidateCount.store(
      countCandidates(true, false), std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapAllCandidateCount.store(
      countCandidates(false, false), std::memory_order_relaxed);

  auto tryResolveUniqueChild =
      [&](bool requireBucketMatch,
          bool requireSourceMetaMatch) -> const ModelDataChildLinkProbeRecord* {
    std::vector<const ModelDataChildLinkProbeRecord*> candidates;
    candidates.reserve(modelDataChildLinks.size());
    for (const auto& childLink : modelDataChildLinks) {
      if (childLink.childModelDataPtr == nullptr)
        continue;
      if (requireBucketMatch && childLink.bucketIndex != bucketIndex)
        continue;
      if (requireSourceMetaMatch && childLink.sourceMeta != sourceMeta)
        continue;
      candidates.push_back(&childLink);
    }

    if (candidates.empty())
      return nullptr;
    if (candidates.size() == 1u)
      return candidates.front();

    void* uniqueModelDataPtr = nullptr;
    bool modelDataConflict = false;
    void* uniqueModelResourcePtr = nullptr;
    bool modelResourceConflict = false;
    const ModelDataChildLinkProbeRecord* modelDataRepresentative = nullptr;
    const ModelDataChildLinkProbeRecord* modelResourceRepresentative = nullptr;

    for (const auto* candidate : candidates) {
      if (candidate == nullptr || candidate->childModelDataPtr == nullptr)
        continue;

      if (uniqueModelDataPtr == nullptr) {
        uniqueModelDataPtr = candidate->childModelDataPtr;
        modelDataRepresentative = candidate;
      } else if (candidate->childModelDataPtr != uniqueModelDataPtr) {
        modelDataConflict = true;
      }

      void* candidateModelResourcePtr =
          NormalizeDirectModelResourcePtr(candidate->childModelDataPtr);
      if (candidateModelResourcePtr == nullptr)
        continue;

      if (uniqueModelResourcePtr == nullptr) {
        uniqueModelResourcePtr = candidateModelResourcePtr;
        modelResourceRepresentative = candidate;
      } else if (candidateModelResourcePtr != uniqueModelResourcePtr) {
        modelResourceConflict = true;
      }
    }

    // anonymous attachment 这条 family 已经证明会把同一 sourceMeta 展成多条
    // child-link；如果它们最终仍指向同一份 child modelData 或同一份 direct
    // model resource，就允许把它视为“语义上唯一”的 child lineage。
    if (!modelDataConflict && modelDataRepresentative != nullptr)
      return modelDataRepresentative;
    if (!modelResourceConflict && modelResourceRepresentative != nullptr)
      return modelResourceRepresentative;
    return nullptr;
  };

  auto tryResolveByRuntimeBucketOrdinal =
      [&]() -> const ModelDataChildLinkProbeRecord* {
    std::vector<RuntimeChildLinkProbeRecord> runtimeChildLinksStorage;
    const std::vector<RuntimeChildLinkProbeRecord>* runtimeChildLinks =
        observedRuntimeLinks;
    if (runtimeChildLinks == nullptr) {
      CollectDirectChildRuntimeLinks(parentRuntimeModelPtr,
                                     runtimeChildLinksStorage);
      runtimeChildLinks = &runtimeChildLinksStorage;
    }
    g_lastAttachmentChildLineageBootstrapRuntimeLinkCount.store(
        uint32_t(std::min<size_t>(runtimeChildLinks->size(), 0xFFFFFFFFu)),
        std::memory_order_relaxed);
    if (runtimeChildLinks->empty())
      return nullptr;

    size_t runtimeBucketOrdinal = 0u;
    bool foundRuntimeChild = false;
    for (const auto& childLink : *runtimeChildLinks) {
      if (childLink.bucketIndex != bucketIndex)
        continue;
      if (childLink.childRuntimeModelPtr == childRuntimeModelPtr) {
        foundRuntimeChild = true;
        break;
      }
      ++runtimeBucketOrdinal;
    }
    g_lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal.store(
        uint32_t(std::min<size_t>(runtimeBucketOrdinal, 0xFFFFFFFFu)),
        std::memory_order_relaxed);
    if (!foundRuntimeChild)
      return nullptr;

    std::vector<const ModelDataChildLinkProbeRecord*> modelDataBucket;
    modelDataBucket.reserve(modelDataChildLinks.size());
    for (const auto& childLink : modelDataChildLinks) {
      if (childLink.bucketIndex == bucketIndex &&
          childLink.childModelDataPtr != nullptr) {
        modelDataBucket.push_back(&childLink);
      }
    }
    g_lastAttachmentChildLineageBootstrapModelDataBucketCount.store(
        uint32_t(std::min<size_t>(modelDataBucket.size(), 0xFFFFFFFFu)),
        std::memory_order_relaxed);
    if (runtimeBucketOrdinal >= modelDataBucket.size())
      return nullptr;

    return modelDataBucket[runtimeBucketOrdinal];
  };

  const ModelDataChildLinkProbeRecord* matchedChild =
      tryResolveByRuntimeBucketOrdinal();
  if (matchedChild != nullptr) {
    g_attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (matchedChild == nullptr)
    matchedChild = tryResolveUniqueChild(true, true);
  if (matchedChild == nullptr)
    matchedChild = tryResolveUniqueChild(false, true);
  if (matchedChild == nullptr)
    matchedChild = tryResolveUniqueChild(true, false);
  if (matchedChild == nullptr)
    matchedChild = tryResolveUniqueChild(false, false);

  if (matchedChild == nullptr) {
    g_attachmentChildLineageBootstrapMissNoUniqueChildCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  outChildModelDataPtr = matchedChild->childModelDataPtr;
  outChildModelResourcePtr =
      NormalizeDirectModelResourcePtr(matchedChild->childModelDataPtr);
  g_attachmentChildLineageBootstrapSuccessCount.fetch_add(
      1u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapChildModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(outChildModelDataPtr)),
      std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapChildModelResourcePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(outChildModelResourcePtr)),
      std::memory_order_relaxed);

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto& resourceCache = ShadowModelResourceCache::instance();
  instanceRegistry.noteRuntimeCreationProvenance(
      childRuntimeModelPtr, outChildModelDataPtr,
      uint32_t(kBuildChildRuntimeModelLinksRva));
  resourceCache.noteRuntimeModelBinding(childRuntimeModelPtr,
                                        outChildModelResourcePtr, 0u);
  if (outChildModelResourcePtr != nullptr)
    resourceCache.noteModelResourceBinding(outChildModelResourcePtr, 0u);

  ModelInstanceRecord childRecord = {};
  if (instanceRegistry.findByRuntimeModel(childRuntimeModelPtr, childRecord) &&
      childRecord.spritePtr != nullptr) {
    ModelRegistry::instance().recordRuntimeModelBinding(
        childRecord.spritePtr, childRuntimeModelPtr, outChildModelResourcePtr,
        0u, 0u);
  }

  return outChildModelDataPtr != nullptr || outChildModelResourcePtr != nullptr;
}

bool LooksLikeChildRuntimeGroupHost(void* candidatePtr) {
  if (candidatePtr == nullptr)
    return false;

  uint32_t childGroupCount = 0u;
  void* childGroupRecords = nullptr;
  if (!SafeReadU32Fast(candidatePtr,
                       dxvk::war3::CModelDataOffsets::ChildRuntimeGroupCount,
                       childGroupCount) ||
      !SafeReadPtrFast(candidatePtr,
                       dxvk::war3::CModelDataOffsets::ChildRuntimeGroupRecords,
                       childGroupRecords)) {
    return false;
  }

  if (childGroupCount == 0u || childGroupCount > 1024u ||
      childGroupRecords == nullptr) {
    return false;
  }

  return dxvk::war3::IsReadableRange(childGroupRecords,
                                     size_t(childGroupCount) * 12u);
}

void* TryScanChildRuntimeGroupHostPtr(void* wrapperPtr) {
  if (wrapperPtr == nullptr)
    return nullptr;

  constexpr size_t kScanLimit = 0x40u;
  for (size_t offset = 0u; offset <= kScanLimit; offset += sizeof(void*)) {
    void* candidatePtr = nullptr;
    if (!SafeReadPtrFast(wrapperPtr, offset, candidatePtr) ||
        candidatePtr == nullptr) {
      continue;
    }
    if (LooksLikeChildRuntimeGroupHost(candidatePtr))
      return candidatePtr;
  }

  return nullptr;
}

void* TryResolveParentModelDataForChildBootstrap(void* candidatePtr) {
  if (LooksLikeChildRuntimeGroupHost(candidatePtr))
    return candidatePtr;

  if (void* scannedPtr = TryScanChildRuntimeGroupHostPtr(candidatePtr);
      scannedPtr != nullptr) {
    return scannedPtr;
  }

  void* nestedModelDataPtr = nullptr;
  if (candidatePtr != nullptr &&
      SafeReadPtrFast(candidatePtr,
                      dxvk::war3::CModelDataOffsets::ModelDataHandle,
                      nestedModelDataPtr) &&
      nestedModelDataPtr != nullptr) {
    if (LooksLikeChildRuntimeGroupHost(nestedModelDataPtr))
      return nestedModelDataPtr;
    if (void* scannedPtr = TryScanChildRuntimeGroupHostPtr(nestedModelDataPtr);
        scannedPtr != nullptr) {
      return scannedPtr;
    }
  }

  return nullptr;
}

bool TryBootstrapAttachmentChildRuntimeLineage(
    void* rootRuntimeModelPtr, void* ownerRuntimeModelPtr,
    void* childRuntimeModelPtr, uint32_t sourceMeta, uint32_t bucketIndex,
    const std::vector<RuntimeChildLinkProbeRecord>* observedRootChildLinks =
        nullptr) {
  if (!LooksLikeRuntimeModelPtr(childRuntimeModelPtr) || sourceMeta == 0u)
    return false;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto tryResolveParentModelDataPtr = [&](void* parentRuntimeModelPtr) {
    if (!LooksLikeRuntimeModelPtr(parentRuntimeModelPtr))
      return static_cast<void*>(nullptr);

    ModelInstanceRecord instanceRecord = {};
    auto tryResolveFromInstanceRecord =
        [&](const ModelInstanceRecord& record) {
          if (void* directModelDataPtr =
                  TryResolveParentModelDataForChildBootstrap(
                      record.runtimeCreatorModelDataPtr);
              directModelDataPtr != nullptr) {
            return directModelDataPtr;
          }
          if (void* directModelDataPtr =
                  TryResolveParentModelDataForChildBootstrap(
                      record.runtimeCreatorHandlePtr);
              directModelDataPtr != nullptr) {
            return directModelDataPtr;
          }
          if (void* directModelDataPtr =
                  TryResolveParentModelDataForChildBootstrap(
                      record.modelResourcePtr);
              directModelDataPtr != nullptr) {
            return directModelDataPtr;
          }
          return static_cast<void*>(nullptr);
        };

    if (instanceRegistry.findByRuntimeModel(parentRuntimeModelPtr,
                                            instanceRecord)) {
      if (void* directModelDataPtr =
              tryResolveFromInstanceRecord(instanceRecord);
          directModelDataPtr != nullptr) {
        return directModelDataPtr;
      }
    }

    instanceRecord = {};
    if (instanceRegistry.findOwnerByRuntimeModel(parentRuntimeModelPtr,
                                                 instanceRecord)) {
      if (void* directModelDataPtr =
              tryResolveFromInstanceRecord(instanceRecord);
          directModelDataPtr != nullptr) {
        return directModelDataPtr;
      }
    }

    void* ownedModelDataHandle = nullptr;
    if (SafeReadPtrFast(parentRuntimeModelPtr,
                        dxvk::war3::CModelOffsets::OwnedModelDataHandle,
                        ownedModelDataHandle) &&
        ownedModelDataHandle != nullptr) {
      if (void* directModelDataPtr =
              TryResolveParentModelDataForChildBootstrap(ownedModelDataHandle);
          directModelDataPtr != nullptr) {
        return directModelDataPtr;
      }
    }

    return static_cast<void*>(nullptr);
  };

  auto tryBootstrapWithModelData = [&](void* runtimeLinkParentPtr,
                                       void* parentModelDataPtr) {
    if (runtimeLinkParentPtr == nullptr || parentModelDataPtr == nullptr)
      return false;

    void* childModelDataPtr = nullptr;
    void* childModelResourcePtr = nullptr;
    return InternalTryBootstrapRuntimeChildLineageFromParentModelData(
        runtimeLinkParentPtr, parentModelDataPtr, childRuntimeModelPtr,
        sourceMeta, bucketIndex, childModelDataPtr, childModelResourcePtr,
        runtimeLinkParentPtr == rootRuntimeModelPtr ? observedRootChildLinks
                                                    : nullptr);
  };

  auto tryBootstrapFromRuntime = [&](void* parentRuntimeModelPtr,
                                     void** outParentModelDataPtr = nullptr) {
    void* parentModelDataPtr =
        tryResolveParentModelDataPtr(parentRuntimeModelPtr);
    if (outParentModelDataPtr != nullptr)
      *outParentModelDataPtr = parentModelDataPtr;
    if (parentModelDataPtr == nullptr)
      return false;
    return tryBootstrapWithModelData(parentRuntimeModelPtr, parentModelDataPtr);
  };

  void* rootModelDataPtr = nullptr;
  if (tryBootstrapFromRuntime(rootRuntimeModelPtr, &rootModelDataPtr))
    return true;
  if (ownerRuntimeModelPtr != rootRuntimeModelPtr) {
    void* ownerModelDataPtr = nullptr;
    if (tryBootstrapFromRuntime(ownerRuntimeModelPtr, &ownerModelDataPtr))
      return true;
    if (ownerModelDataPtr != nullptr &&
        tryBootstrapWithModelData(rootRuntimeModelPtr, ownerModelDataPtr)) {
      return true;
    }
    if (rootModelDataPtr != nullptr &&
        tryBootstrapWithModelData(ownerRuntimeModelPtr, rootModelDataPtr)) {
      return true;
    }
  }
  return false;
}

bool TryCollectContextRuntimeWithChildLinks(
    const void* contextPtr, RuntimeContextChildLinkProbeRecord& out,
    std::vector<RuntimeChildLinkProbeRecord>& outChildLinks) {
  out = {};
  outChildLinks.clear();
  if (contextPtr == nullptr)
    return false;

  constexpr uint32_t kContextScanLimit = 0x40u;
  constexpr uint32_t kPointerStride = sizeof(void*);
  for (uint32_t offset = 0u; offset <= kContextScanLimit;
       offset += kPointerStride) {
    void* candidate = TryReadPtrFast(contextPtr, offset);
    if (!LooksLikeRuntimeModelPtr(candidate))
      continue;

    std::vector<RuntimeChildLinkProbeRecord> candidateChildLinks;
    CollectDirectChildRuntimeLinks(candidate, candidateChildLinks);
    if (candidateChildLinks.empty())
      continue;

    if (candidateChildLinks.size() <= outChildLinks.size())
      continue;

    out.runtimeModelPtr = candidate;
    out.offset = offset;
    out.maxTag = 0u;
    for (const auto& childLink : candidateChildLinks)
      out.maxTag = std::max(out.maxTag, childLink.tag);
    outChildLinks = std::move(candidateChildLinks);
  }

  return out.runtimeModelPtr != nullptr;
}

bool TryCollectRuntimeWithChildLinksFromBlock(
    const void* blockPtr, uint32_t scanLimit,
    RuntimeContextChildLinkProbeRecord& out,
    std::vector<RuntimeChildLinkProbeRecord>& outChildLinks) {
  out = {};
  outChildLinks.clear();
  if (blockPtr == nullptr)
    return false;

  constexpr uint32_t kPointerStride = sizeof(void*);
  for (uint32_t offset = 0u; offset <= scanLimit; offset += kPointerStride) {
    void* candidate = TryReadPtrFast(blockPtr, offset);
    if (!LooksLikeRuntimeModelPtr(candidate))
      continue;

    std::vector<RuntimeChildLinkProbeRecord> candidateChildLinks;
    CollectDirectChildRuntimeLinks(candidate, candidateChildLinks);
    if (candidateChildLinks.empty())
      continue;

    if (candidateChildLinks.size() <= outChildLinks.size())
      continue;

    out.runtimeModelPtr = candidate;
    out.offset = offset;
    out.maxTag = 0u;
    for (const auto& childLink : candidateChildLinks)
      out.maxTag = std::max(out.maxTag, childLink.tag);
    outChildLinks = std::move(candidateChildLinks);
  }

  return out.runtimeModelPtr != nullptr;
}

void* TryResolveScratchRootRuntimeModel(const void* contextPtr) {
  if (contextPtr == nullptr)
    return nullptr;
  void* scratchRootPtr = TryReadPtrFast(contextPtr, 72u);
  if (scratchRootPtr == nullptr)
    return nullptr;

  const uintptr_t scratchRootValue = reinterpret_cast<uintptr_t>(scratchRootPtr);
  if (scratchRootValue < 0xFCu)
    return nullptr;

  void* runtimeModelPtr =
      reinterpret_cast<void*>(scratchRootValue - 0xFCu);
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return nullptr;
  return runtimeModelPtr;
}

void* TryResolveRootRuntimeModelFromArgBlock(const void* contextPtr) {
  if (contextPtr == nullptr)
    return nullptr;
  void* argBlockPtr = TryReadPtrFast(contextPtr, 72u);
  if (argBlockPtr == nullptr)
    return nullptr;

  void* runtimeModelPtr = TryReadPtrFast(argBlockPtr, 0x1Cu);
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return nullptr;
  return runtimeModelPtr;
}

void MergeAttachmentIdentityFromInstance(ModelInstanceRecord& dst,
                                         const ModelInstanceRecord& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.spritePtr == nullptr)
    dst.spritePtr = src.spritePtr;
  if (dst.runtimeModelPtr == nullptr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (dst.sourceObjectPtr == nullptr)
    dst.sourceObjectPtr = src.sourceObjectPtr;
  if (dst.sourceSpriteObjectPtr == nullptr)
    dst.sourceSpriteObjectPtr = src.sourceSpriteObjectPtr;
  if (dst.modelResourcePtr == nullptr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
  if (dst.modelKey == 0u)
    dst.modelKey = src.modelKey;
}

void MergeAttachmentIdentityFromShadow(
    ModelInstanceRecord& dst,
    const render::ShadowObjectRecord& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.spritePtr == nullptr)
    dst.spritePtr = src.spritePtr;
  if (dst.runtimeModelPtr == nullptr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (dst.modelResourcePtr == nullptr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
  if (dst.modelKey == 0u)
    dst.modelKey = src.modelKey;
}

void MergeAttachmentIdentityFromRender(
    ModelInstanceRecord& dst,
    const render::RenderObjectIdentitySnapshot& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
}

void MergeAttachmentIdentityFromPose(ModelInstanceRecord& dst,
                                     const PoseRecord& src) {
  if (dst.runtimeModelPtr == nullptr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.spritePtr == nullptr)
    dst.spritePtr = src.spritePtr;
}

bool HasAttachmentIdentity(const ModelInstanceRecord& record) {
  return record.worldObjectEntry != nullptr ||
         record.sceneNode != nullptr ||
         record.unitPtr != nullptr ||
         record.jHandle != 0u ||
         record.rawcode != 0u;
}

bool HasLogicalObjectIdentity(const ModelInstanceRecord& record) {
  return record.unitPtr != nullptr || record.jHandle != 0u ||
         record.rawcode != 0u;
}

bool HasAttachmentSourceHint(const ModelInstanceRecord& record) {
  return record.sourceObjectPtr != nullptr ||
         record.sourceSpriteObjectPtr != nullptr;
}

uint32_t ScoreAttachmentOwnerRuntimeCandidate(
    void* candidateRuntimeModelPtr, void* rootRuntimeModelPtr,
    void* currentRuntimeModelPtr) {
  if (!LooksLikeRuntimeModelPtr(candidateRuntimeModelPtr))
    return 0u;

  uint32_t score = 1u;
  const uintptr_t candidateValue =
      reinterpret_cast<uintptr_t>(candidateRuntimeModelPtr);

  ModelInstanceRecord instanceRecord = {};
  const auto& instanceRegistry = ModelInstanceRegistry::instance();
  if (instanceRegistry.findByRuntimeModel(candidateRuntimeModelPtr,
                                          instanceRecord)) {
    score += 4u;
    if (instanceRecord.runtimeCreatorCallerRva != 0u ||
        instanceRecord.runtimeCreatorModelDataPtr != nullptr) {
      score += 8u;
    }
    if (instanceRecord.runtimeResolveCallerRva != 0u ||
        instanceRecord.runtimeCreatorHandlePtr != nullptr) {
      score += 4u;
    }
    if (instanceRecord.sourceObjectPtr != nullptr ||
        instanceRecord.sourceSpriteObjectPtr != nullptr) {
      score += 6u;
    }
    if (instanceRecord.spritePtr != nullptr)
      score += 4u;
    if (HasAttachmentIdentity(instanceRecord))
      score += 8u;
    if (instanceRecord.modelResourcePtr != nullptr || instanceRecord.modelKey != 0u)
      score += 2u;
  }

  if (instanceRegistry.findOwnerByRuntimeModel(candidateRuntimeModelPtr,
                                               instanceRecord)) {
    score += 6u;
    if (instanceRecord.sourceObjectPtr != nullptr ||
        instanceRecord.sourceSpriteObjectPtr != nullptr) {
      score += 6u;
    }
    if (instanceRecord.spritePtr != nullptr)
      score += 2u;
    if (HasAttachmentIdentity(instanceRecord))
      score += 8u;
  }

  ModelResourceRecord modelRecord = {};
  if (ModelRegistry::instance().findByRuntimeModel(candidateRuntimeModelPtr,
                                                   modelRecord)) {
    score += 3u;
    if (modelRecord.modelResourcePtr != nullptr)
      score += 2u;
    if (modelRecord.spritePtr != nullptr)
      score += 1u;
  }

  PoseRecord poseRecord = {};
  if (PoseRegistry::instance().findByRuntimeModel(candidateRuntimeModelPtr,
                                                  poseRecord)) {
    score += 3u;
    if (poseRecord.matrixCount != 0u)
      score += 2u;
    if (poseRecord.spritePtr != nullptr)
      score += 1u;
  }

  if (candidateRuntimeModelPtr == currentRuntimeModelPtr)
    score += 2u;
  if (candidateRuntimeModelPtr == rootRuntimeModelPtr)
    score += 1u;

  // 这类低地址假命中目前主要出现在 arg-block scan 里；如果没有足够的
  // registry/provenance 证据，直接把它降权到不可选，避免污染 attachment contract。
  if (candidateValue < 0x01000000u && score < 16u)
    return 0u;
  return score;
}

void* ChooseAttachmentOwnerRuntimeModel(
    void* rootRuntimeModelPtr, void* currentRuntimeModelPtr,
    void* argBlockRuntimeModelPtr, void* arg4BlockRuntimeModelPtr) {
  const std::array<void*, 4u> candidates = {
      argBlockRuntimeModelPtr,
      arg4BlockRuntimeModelPtr,
      currentRuntimeModelPtr,
      rootRuntimeModelPtr,
  };

  void* bestRuntimeModelPtr = nullptr;
  uint32_t bestScore = 0u;
  for (void* candidateRuntimeModelPtr : candidates) {
    const uint32_t candidateScore =
        ScoreAttachmentOwnerRuntimeCandidate(candidateRuntimeModelPtr,
                                            rootRuntimeModelPtr,
                                            currentRuntimeModelPtr);
    if (candidateScore <= bestScore)
      continue;
    bestRuntimeModelPtr = candidateRuntimeModelPtr;
    bestScore = candidateScore;
  }

  if (bestRuntimeModelPtr != nullptr)
    return bestRuntimeModelPtr;
  if (LooksLikeRuntimeModelPtr(currentRuntimeModelPtr))
    return currentRuntimeModelPtr;
  if (LooksLikeRuntimeModelPtr(rootRuntimeModelPtr))
    return rootRuntimeModelPtr;
  if (LooksLikeRuntimeModelPtr(arg4BlockRuntimeModelPtr))
    return arg4BlockRuntimeModelPtr;
  if (LooksLikeRuntimeModelPtr(argBlockRuntimeModelPtr))
    return argBlockRuntimeModelPtr;
  return nullptr;
}

void MergeAttachmentHintsFromAncestorRuntimes(
    void* runtimeModelPtr, ModelInstanceRecord& ioRecord,
    bool& outResolvedStrongIdentity, uint32_t& outDepth,
    void*& outAncestorRuntimeModelPtr) {
  outResolvedStrongIdentity = false;
  outDepth = 0u;
  outAncestorRuntimeModelPtr = nullptr;
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  std::vector<void*> ancestors;
  ancestors.reserve(8u);
  {
    std::lock_guard<std::mutex> lock(g_runtimeParentLinkMutex);
    std::unordered_set<void*> visited;
    visited.reserve(8u);
    void* currentRuntimeModelPtr = runtimeModelPtr;
    constexpr uint32_t kMaxAncestorDepth = 8u;
    for (uint32_t depth = 0u; depth < kMaxAncestorDepth; ++depth) {
      const auto it = g_runtimeParentLinks.find(currentRuntimeModelPtr);
      if (it == g_runtimeParentLinks.end())
        break;

      void* parentRuntimeModelPtr = it->second.parentRuntimeModelPtr;
      if (!LooksLikeRuntimeModelPtr(parentRuntimeModelPtr) ||
          !visited.insert(parentRuntimeModelPtr).second) {
        break;
      }

      ancestors.push_back(parentRuntimeModelPtr);
      currentRuntimeModelPtr = parentRuntimeModelPtr;
    }
  }

  if (ancestors.empty())
    return;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  auto& poseRegistry = PoseRegistry::instance();
  for (size_t index = 0u; index < ancestors.size(); ++index) {
    void* ancestorRuntimeModelPtr = ancestors[index];
    ModelInstanceRecord ancestorRecord = {};
    ModelInstanceRecord instanceRecord = {};
    if (instanceRegistry.findByRuntimeModel(ancestorRuntimeModelPtr,
                                            instanceRecord)) {
      MergeAttachmentIdentityFromInstance(ancestorRecord, instanceRecord);
    }
    if (instanceRegistry.findOwnerByRuntimeModel(ancestorRuntimeModelPtr,
                                                 instanceRecord)) {
      MergeAttachmentIdentityFromInstance(ancestorRecord, instanceRecord);
    }

    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findByRuntimeModel(ancestorRuntimeModelPtr,
                                          shadowRecord)) {
      MergeAttachmentIdentityFromShadow(ancestorRecord, shadowRecord);
    }

    PoseRecord poseRecord = {};
    if (poseRegistry.findByRuntimeModel(ancestorRuntimeModelPtr, poseRecord))
      MergeAttachmentIdentityFromPose(ancestorRecord, poseRecord);

    if (ancestorRecord.runtimeModelPtr == nullptr)
      ancestorRecord.runtimeModelPtr = ancestorRuntimeModelPtr;

    if (!HasAttachmentIdentity(ancestorRecord) &&
        !HasAttachmentSourceHint(ancestorRecord)) {
      continue;
    }

    const bool hadStrongIdentity = HasAttachmentIdentity(ioRecord);
    const bool hadSourceHint = HasAttachmentSourceHint(ioRecord);
    MergeAttachmentIdentityFromInstance(ioRecord, ancestorRecord);
    if (!hadStrongIdentity && HasAttachmentIdentity(ioRecord))
      outResolvedStrongIdentity = true;
    if (!hadStrongIdentity && HasAttachmentIdentity(ioRecord)) {
      outDepth = uint32_t(index + 1u);
      outAncestorRuntimeModelPtr = ancestorRuntimeModelPtr;
    } else if (!hadSourceHint && HasAttachmentSourceHint(ioRecord) &&
               outAncestorRuntimeModelPtr == nullptr) {
      outDepth = uint32_t(index + 1u);
      outAncestorRuntimeModelPtr = ancestorRuntimeModelPtr;
    }
  }
}

void RecordRuntimeChildLinkBuild(
    void* parentRuntimeModelPtr, void* modelDataPtr,
    const std::vector<ModelDataChildLinkProbeRecord>* capturedModelDataLinks =
        nullptr) {
  if (parentRuntimeModelPtr == nullptr)
    return;

  g_runtimeChildLinkBuildCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastRuntimeChildLinkBuildParentRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(parentRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeChildLinkBuildModelDataPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(modelDataPtr)),
      std::memory_order_relaxed);

  std::vector<RuntimeChildLinkProbeRecord> childLinks;
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelRuntimeChildCollect);
    CollectDirectChildRuntimeLinks(parentRuntimeModelPtr, childLinks);
  }
  if (childLinks.empty())
    return;

  if (capturedModelDataLinks == nullptr || !capturedModelDataLinks->empty()) {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelRuntimeChildBootstrap);
    BootstrapRuntimeChildLineageFromModelData(
        parentRuntimeModelPtr, modelDataPtr, childLinks, capturedModelDataLinks);
  }

  g_runtimeChildLinkBuiltChildCount.fetch_add(
      uint64_t(childLinks.size()), std::memory_order_relaxed);

  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelRuntimeChildParentMap);
    std::lock_guard<std::mutex> lock(g_runtimeParentLinkMutex);
    // Phase 7.83：PoseRegistry::frameNumber 已 atomic，用单次 load 替代
    // ternary 双读。
    uint64_t frame = PoseRegistry::instance().frameNumber();
    if (frame == 0u)
      frame = ModelInstanceRegistry::instance().frameNumber();
    g_runtimeParentLinks.reserve(g_runtimeParentLinks.size() + childLinks.size());
    for (const auto& childLink : childLinks) {
      if (!LooksLikeRuntimeModelPtr(childLink.childRuntimeModelPtr))
        continue;
      RuntimeParentLinkRecord& record =
          g_runtimeParentLinks[childLink.childRuntimeModelPtr];
      record.parentRuntimeModelPtr = parentRuntimeModelPtr;
      record.sourceMeta = childLink.sourceMeta;
      record.bucketIndex = childLink.bucketIndex;
      record.lastSeenFrame = frame;
    }
  }

  const auto sampleLink = std::find_if(
      childLinks.begin(), childLinks.end(),
      [](const RuntimeChildLinkProbeRecord& record) {
        return LooksLikeRuntimeModelPtr(record.childRuntimeModelPtr);
      });
  if (sampleLink != childLinks.end()) {
    g_lastRuntimeChildLinkBuildChildRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(sampleLink->childRuntimeModelPtr)),
        std::memory_order_relaxed);
    g_lastRuntimeChildLinkBuildSourceMeta.store(sampleLink->sourceMeta,
                                                std::memory_order_relaxed);
  }

  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelRuntimeChildOwnerPropagate);
    auto& instanceRegistry = ModelInstanceRegistry::instance();
    ModelInstanceRecord parentRecord = {};
    if (!instanceRegistry.findByRuntimeModel(parentRuntimeModelPtr,
                                             parentRecord)) {
      instanceRegistry.findOwnerByRuntimeModel(parentRuntimeModelPtr,
                                               parentRecord);
    }
    if (!HasAttachmentIdentity(parentRecord) &&
        !HasAttachmentSourceHint(parentRecord)) {
      return;
    }

    for (const auto& childLink : childLinks) {
      if (!LooksLikeRuntimeModelPtr(childLink.childRuntimeModelPtr))
        continue;
      if (HasAttachmentSourceHint(parentRecord)) {
        instanceRegistry.noteRuntimeSourceObject(
            childLink.childRuntimeModelPtr, parentRecord.sourceObjectPtr,
            parentRecord.sourceSpriteObjectPtr, parentRecord.spritePtr);
      }
      if (HasAttachmentIdentity(parentRecord)) {
        instanceRegistry.noteRuntimeOwnerIdentity(
            childLink.childRuntimeModelPtr, parentRecord.worldObjectEntry,
            parentRecord.sceneNode, parentRecord.unitPtr,
            parentRecord.spritePtr, parentRecord.jHandle,
            parentRecord.rawcode);
      }
    }
  }
}

void RecordObservedRuntimeChildLink(void* parentRuntimeModelPtr,
                                    void* childRuntimeModelPtr,
                                    uint32_t sourceMeta,
                                    uint32_t bucketIndex) {
  if (!LooksLikeRuntimeModelPtr(parentRuntimeModelPtr) ||
      !LooksLikeRuntimeModelPtr(childRuntimeModelPtr)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_runtimeParentLinkMutex);
    // Phase 7.83：单次 load + fallback。
    uint64_t frame = PoseRegistry::instance().frameNumber();
    if (frame == 0u)
      frame = ModelInstanceRegistry::instance().frameNumber();
    RuntimeParentLinkRecord& record = g_runtimeParentLinks[childRuntimeModelPtr];
    record.parentRuntimeModelPtr = parentRuntimeModelPtr;
    record.sourceMeta = sourceMeta;
    record.bucketIndex = bucketIndex;
    record.lastSeenFrame = frame;
  }

  BootstrapRuntimeModelResourceLineage(parentRuntimeModelPtr);
  BootstrapRuntimeModelResourceLineage(childRuntimeModelPtr);

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  ModelInstanceRecord parentRecord = {};
  if (!instanceRegistry.findByRuntimeModel(parentRuntimeModelPtr, parentRecord))
    instanceRegistry.findOwnerByRuntimeModel(parentRuntimeModelPtr, parentRecord);
  if (!HasAttachmentIdentity(parentRecord) &&
      !HasAttachmentSourceHint(parentRecord)) {
    return;
  }

  if (HasAttachmentSourceHint(parentRecord)) {
    instanceRegistry.noteRuntimeSourceObject(
        childRuntimeModelPtr, parentRecord.sourceObjectPtr,
        parentRecord.sourceSpriteObjectPtr, parentRecord.spritePtr);
  }
  if (HasAttachmentIdentity(parentRecord)) {
    instanceRegistry.noteRuntimeOwnerIdentity(
        childRuntimeModelPtr, parentRecord.worldObjectEntry,
        parentRecord.sceneNode, parentRecord.unitPtr, parentRecord.spritePtr,
        parentRecord.jHandle, parentRecord.rawcode);
  }
}

bool TryResolveCurrentRenderOwnerHint(void* spritePtr, void* runtimeModelPtr,
                                      ModelInstanceRecord& out,
                                      render::ObjectKind& outKind) {
  g_currentRenderIdentityHintCount.fetch_add(1u, std::memory_order_relaxed);

  const auto& semanticState = War3RenderState::GetTlsShadowSemanticState();
  const render::RenderObjectInfo* currentBatchObject =
      render::GetCurrentBatchObject();
  const render::RenderObjectInfo* semanticObject = semanticState.object;
  if (semanticState.HasAnyContext() || currentBatchObject != nullptr) {
    if (out.worldObjectEntry == nullptr) {
      out.worldObjectEntry = semanticState.worldObjectEntry != nullptr
                                 ? semanticState.worldObjectEntry
                                 : (currentBatchObject != nullptr
                                        ? currentBatchObject->worldObjectEntry
                                        : nullptr);
    }
    if (out.sceneNode == nullptr) {
      out.sceneNode = semanticState.sceneNode != nullptr
                          ? semanticState.sceneNode
                          : (currentBatchObject != nullptr
                                 ? currentBatchObject->sceneNode
                                 : nullptr);
    }
    if (out.unitPtr == nullptr) {
      out.unitPtr = semanticObject != nullptr
                        ? semanticObject->unitPtr
                        : (currentBatchObject != nullptr
                               ? currentBatchObject->unitPtr
                               : nullptr);
    }
    if (out.jHandle == 0u) {
      out.jHandle = semanticState.jHandle != 0u
                        ? semanticState.jHandle
                        : (currentBatchObject != nullptr
                               ? currentBatchObject->jHandle
                               : 0u);
    }
    if (out.rawcode == 0u) {
      out.rawcode = semanticState.rawcode != 0u
                        ? semanticState.rawcode
                        : (currentBatchObject != nullptr
                               ? currentBatchObject->rawcode
                               : 0u);
    }
    if (spritePtr != nullptr && out.spritePtr == nullptr)
      out.spritePtr = spritePtr;
    if (runtimeModelPtr != nullptr && out.runtimeModelPtr == nullptr)
      out.runtimeModelPtr = runtimeModelPtr;
    if (outKind == render::ObjectKind::Unknown) {
      if (static_cast<uint32_t>(semanticState.objectKind) != 0u)
        outKind = semanticState.objectKind;
      else if (currentBatchObject != nullptr &&
               currentBatchObject->kind != render::ObjectKind::Unknown)
        outKind = currentBatchObject->kind;
    }

    if (HasAttachmentIdentity(out)) {
      g_currentRenderIdentityResolvedCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastCurrentRenderIdentityWorldObjectEntryPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(out.worldObjectEntry)),
          std::memory_order_relaxed);
      g_lastCurrentRenderIdentitySceneNodePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(out.sceneNode)),
          std::memory_order_relaxed);
      g_lastCurrentRenderIdentityUnitPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(out.unitPtr)),
          std::memory_order_relaxed);
      return true;
    }
  }

  render::RenderObjectIdentitySnapshot snapshot = {};
  if (!render::TryResolveCurrentRenderObjectIdentity(out.sceneNode, snapshot) ||
      !HasResolvedRenderIdentity(snapshot)) {
    return false;
  }

  g_currentRenderIdentityResolvedCount.fetch_add(1u,
                                                 std::memory_order_relaxed);
  g_lastCurrentRenderIdentityWorldObjectEntryPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(snapshot.worldObjectEntry)),
      std::memory_order_relaxed);
  g_lastCurrentRenderIdentitySceneNodePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(snapshot.sceneNode)),
      std::memory_order_relaxed);
  g_lastCurrentRenderIdentityUnitPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(snapshot.unitPtr)),
      std::memory_order_relaxed);

  MergeAttachmentIdentityFromRender(out, snapshot);
  if (spritePtr != nullptr && out.spritePtr == nullptr)
    out.spritePtr = spritePtr;
  if (runtimeModelPtr != nullptr && out.runtimeModelPtr == nullptr)
    out.runtimeModelPtr = runtimeModelPtr;
  if (outKind == render::ObjectKind::Unknown &&
      snapshot.kind != render::ObjectKind::Unknown) {
    outKind = snapshot.kind;
  }

  return HasAttachmentIdentity(out);
}

bool TryResolveParentSpriteOwnerHint(void* spritePtr,
                                     void* currentRuntimeModelPtr,
                                     ModelInstanceRecord& out) {
  out = {};
  if (spritePtr == nullptr)
    return false;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  std::unordered_set<void*> visitedSprites;
  visitedSprites.reserve(8u);

  void* currentSprite =
      TryReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::ParentSprite);
  constexpr uint32_t kMaxParentDepth = 8u;
  for (uint32_t depth = 0u;
       currentSprite != nullptr && depth < kMaxParentDepth &&
       visitedSprites.insert(currentSprite).second;
       ++depth) {
    ModelInstanceRecord candidate = {};
    if (instanceRegistry.findBySpritePtr(currentSprite, candidate) &&
        HasAttachmentIdentity(candidate)) {
      if (candidate.spritePtr == nullptr)
        candidate.spritePtr = currentSprite;
      if (candidate.runtimeModelPtr == nullptr)
        candidate.runtimeModelPtr = currentRuntimeModelPtr;
      out = candidate;
      return true;
    }

    void* parentRuntimeModelPtr =
        TryReadPtrFast(currentSprite, dxvk::war3::CSpriteOffsets::Model);
    if (instanceRegistry.findOwnerByRuntimeModel(parentRuntimeModelPtr,
                                                 candidate) &&
        HasAttachmentIdentity(candidate)) {
      if (candidate.spritePtr == nullptr)
        candidate.spritePtr = currentSprite;
      if (candidate.runtimeModelPtr == nullptr)
        candidate.runtimeModelPtr = currentRuntimeModelPtr;
      out = candidate;
      return true;
    }
    if (instanceRegistry.findByRuntimeModel(parentRuntimeModelPtr, candidate) &&
        HasAttachmentIdentity(candidate)) {
      if (candidate.spritePtr == nullptr)
        candidate.spritePtr = currentSprite;
      if (candidate.runtimeModelPtr == nullptr)
        candidate.runtimeModelPtr = currentRuntimeModelPtr;
      out = candidate;
      return true;
    }

    currentSprite =
        TryReadPtrFast(currentSprite, dxvk::war3::CSpriteOffsets::ParentSprite);
  }

  return false;
}

bool TryResolveAttachmentIdentity(void* rootRuntimeModelPtr,
                                  void* childRuntimeModelPtr,
                                  void* currentRuntimeModelPtr,
                                  void* argBlockRuntimeModelPtr,
                                  void* arg4BlockRuntimeModelPtr,
                                  ModelInstanceRecord& out) {
  out = {};

  const std::array<void*, 5u> candidates = {
      rootRuntimeModelPtr,
      childRuntimeModelPtr,
      currentRuntimeModelPtr,
      argBlockRuntimeModelPtr,
      arg4BlockRuntimeModelPtr,
  };

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    ModelInstanceRecord instanceRecord = {};
    if (!instanceRegistry.findByRuntimeModel(candidate, instanceRecord))
      continue;
    MergeAttachmentIdentityFromInstance(out, instanceRecord);
  }
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    ModelInstanceRecord sceneRecord = {};
    if (!instanceRegistry.findBySceneNode(candidate, sceneRecord))
      continue;
    MergeAttachmentIdentityFromInstance(out, sceneRecord);
  }
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    ModelInstanceRecord ownerRecord = {};
    if (!instanceRegistry.findOwnerByRuntimeModel(candidate, ownerRecord))
      continue;
    MergeAttachmentIdentityFromInstance(out, ownerRecord);
  }

  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    render::ShadowObjectRecord shadowRecord = {};
    if (!shadowRegistry.findByRuntimeModel(candidate, shadowRecord))
      continue;
    MergeAttachmentIdentityFromShadow(out, shadowRecord);
  }
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    render::ShadowObjectRecord shadowRecord = {};
    if (!shadowRegistry.findBySceneNode(candidate, shadowRecord))
      continue;
    MergeAttachmentIdentityFromShadow(out, shadowRecord);
  }

  auto& poseRegistry = PoseRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    PoseRecord poseRecord = {};
    if (!poseRegistry.findByRuntimeModel(candidate, poseRecord))
      continue;
    MergeAttachmentIdentityFromPose(out, poseRecord);
  }
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;
    PoseRecord poseRecord = {};
    if (!poseRegistry.findBySceneNode(candidate, poseRecord))
      continue;
    MergeAttachmentIdentityFromPose(out, poseRecord);
  }
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;

    ModelInstanceRecord candidateRecord = {};
    if (!TryResolveIdentityFromPointerCandidate(candidate, childRuntimeModelPtr,
                                                candidateRecord)) {
      continue;
    }

    MergeAttachmentIdentityFromInstance(out, candidateRecord);
  }

  if (out.worldObjectEntry == nullptr && out.sceneNode != nullptr) {
    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findBySceneNode(out.sceneNode, shadowRecord))
      MergeAttachmentIdentityFromShadow(out, shadowRecord);
  }
  if (out.sceneNode == nullptr && out.worldObjectEntry != nullptr) {
    render::RenderObjectIdentitySnapshot renderIdentity = {};
    if (render::TryResolveRenderObjectIdentity(out.worldObjectEntry, nullptr,
                                               renderIdentity) &&
        HasUsableAttachmentRenderIdentity(renderIdentity)) {
      MergeAttachmentIdentityFromRender(out, renderIdentity);
    }
  }
  if (out.worldObjectEntry == nullptr && out.sceneNode != nullptr) {
    render::RenderObjectIdentitySnapshot renderIdentity = {};
    if (render::TryResolveRenderObjectIdentity(nullptr, out.sceneNode,
                                               renderIdentity) &&
        HasUsableAttachmentRenderIdentity(renderIdentity)) {
      MergeAttachmentIdentityFromRender(out, renderIdentity);
    }
  }
  if (out.worldObjectEntry == nullptr && out.unitPtr != nullptr) {
    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findByUnitPtr(out.unitPtr, shadowRecord))
      MergeAttachmentIdentityFromShadow(out, shadowRecord);
  }

  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;

    bool resolvedStrongIdentity = false;
    uint32_t ancestorDepth = 0u;
    void* ancestorRuntimeModelPtr = nullptr;
    const bool hadStrongIdentity = HasAttachmentIdentity(out);
    MergeAttachmentHintsFromAncestorRuntimes(candidate, out,
                                             resolvedStrongIdentity,
                                             ancestorDepth,
                                             ancestorRuntimeModelPtr);
    if (!hadStrongIdentity && resolvedStrongIdentity) {
      g_attachmentAncestorIdentityHintWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastAttachmentAncestorFromRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(candidate)),
          std::memory_order_relaxed);
      g_lastAttachmentAncestorRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(ancestorRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastAttachmentAncestorDepth.store(ancestorDepth,
                                          std::memory_order_relaxed);
    }
  }

  return HasAttachmentIdentity(out);
}

bool HasStrongAttachmentIdentity(const ModelInstanceRecord& record) {
  return record.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
         record.unitPtr != nullptr ||
         (record.jHandle != 0u && record.rawcode != 0u);
}

uint32_t AttachmentIdentityScore(const ModelInstanceRecord& record) {
  uint32_t score = 0u;
  if (record.worldObjectEntry != nullptr)
    score += 32u;
  if (record.sceneNode != nullptr)
    score += 24u;
  if (record.unitPtr != nullptr)
    score += 24u;
  if (record.spritePtr != nullptr)
    score += 8u;
  if (record.runtimeModelPtr != nullptr)
    score += 4u;
  if (record.jHandle != 0u)
    score += 4u;
  if (record.rawcode != 0u)
    score += 2u;
  return score;
}

bool TryResolveIdentityFromPointerCandidate(void* candidatePtr,
                                            void* preferredRuntimeModelPtr,
                                            ModelInstanceRecord& out) {
  out = {};
  if (candidatePtr == nullptr)
    return false;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findByWorldObjectEntry(candidatePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(out, instanceRecord);
  if (instanceRegistry.findBySceneNode(candidatePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(out, instanceRecord);
  if (instanceRegistry.findByUnitPtr(candidatePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(out, instanceRecord);
  if (instanceRegistry.findBySpritePtr(candidatePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(out, instanceRecord);
  if (instanceRegistry.findOwnerByRuntimeModel(candidatePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(out, instanceRecord);
  if (instanceRegistry.findByRuntimeModel(candidatePtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(out, instanceRecord);

  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findByWorldObjectEntry(candidatePtr, shadowRecord))
    MergeAttachmentIdentityFromShadow(out, shadowRecord);
  if (shadowRegistry.findBySceneNode(candidatePtr, shadowRecord))
    MergeAttachmentIdentityFromShadow(out, shadowRecord);
  if (shadowRegistry.findByUnitPtr(candidatePtr, shadowRecord))
    MergeAttachmentIdentityFromShadow(out, shadowRecord);
  if (shadowRegistry.findBySpritePtr(candidatePtr, shadowRecord))
    MergeAttachmentIdentityFromShadow(out, shadowRecord);
  if (shadowRegistry.findByRuntimeModel(candidatePtr, shadowRecord))
    MergeAttachmentIdentityFromShadow(out, shadowRecord);

  if (const render::RenderObjectInfo* renderObject =
          render::RenderObjectRegistry::instance().findByEntry(candidatePtr)) {
    const auto snapshot = render::MakeRenderObjectIdentitySnapshot(*renderObject);
    if (HasUsableAttachmentRenderIdentity(snapshot))
      MergeAttachmentIdentityFromRender(out, snapshot);
  }
  if (const render::RenderObjectInfo* renderObject =
          render::RenderObjectRegistry::instance().findBySceneNode(
              candidatePtr)) {
    const auto snapshot = render::MakeRenderObjectIdentitySnapshot(*renderObject);
    if (HasUsableAttachmentRenderIdentity(snapshot))
      MergeAttachmentIdentityFromRender(out, snapshot);
  }

  render::RenderObjectIdentitySnapshot renderIdentity = {};
  if (render::TryResolveRenderObjectIdentity(candidatePtr, nullptr,
                                             renderIdentity) &&
      HasUsableAttachmentRenderIdentity(renderIdentity)) {
    MergeAttachmentIdentityFromRender(out, renderIdentity);
  }

  renderIdentity = {};
  if (render::TryResolveRenderObjectIdentity(nullptr, candidatePtr,
                                             renderIdentity) &&
      HasUsableAttachmentRenderIdentity(renderIdentity)) {
    MergeAttachmentIdentityFromRender(out, renderIdentity);
  }

  if (out.runtimeModelPtr == nullptr)
    out.runtimeModelPtr = preferredRuntimeModelPtr;
  return HasStrongAttachmentIdentity(out);
}

bool TryResolveAttachmentIdentityFromBlock(const void* blockPtr,
                                          uint32_t scanLimit,
                                          void* preferredRuntimeModelPtr,
                                          ModelInstanceRecord& out,
                                          uint32_t& outOffset,
                                          void*& outCandidatePtr) {
  out = {};
  outOffset = 0u;
  outCandidatePtr = nullptr;
  if (blockPtr == nullptr)
    return false;

  constexpr uint32_t kPointerStride = sizeof(void*);
  ModelInstanceRecord bestRecord = {};
  uint32_t bestScore = 0u;
  uint32_t bestOffset = 0u;
  void* bestCandidate = nullptr;
  for (uint32_t offset = 0u; offset <= scanLimit; offset += kPointerStride) {
    void* candidatePtr = TryReadPtrFast(blockPtr, offset);
    ModelInstanceRecord candidateRecord = {};
    if (!TryResolveIdentityFromPointerCandidate(candidatePtr,
                                                preferredRuntimeModelPtr,
                                                candidateRecord)) {
      continue;
    }

    const uint32_t score = AttachmentIdentityScore(candidateRecord);
    if (bestCandidate != nullptr && score <= bestScore)
      continue;

    bestRecord = candidateRecord;
    bestScore = score;
    bestOffset = offset;
    bestCandidate = candidatePtr;
  }

  if (bestCandidate == nullptr)
    return false;

  out = bestRecord;
  outOffset = bestOffset;
  outCandidatePtr = bestCandidate;
  return true;
}

bool TryResolveSourceObjectIdentityHint(void* sourceObjectPtr,
                                        void* preferredRuntimeModelPtr,
                                        ModelInstanceRecord& out,
                                        uint32_t& outOffset,
                                        void*& outCandidatePtr) {
  out = {};
  outOffset = 0u;
  outCandidatePtr = nullptr;
  if (sourceObjectPtr == nullptr)
    return false;
  if (!IsReadableRange(reinterpret_cast<const uint8_t*>(sourceObjectPtr),
                       sizeof(void*))) {
    return false;
  }

  return TryResolveAttachmentIdentityFromBlock(
      sourceObjectPtr, 0x40u, preferredRuntimeModelPtr,
      out, outOffset, outCandidatePtr);
}

bool TryResolveSourceObjectIdentityHintDeep(void* sourceObjectPtr,
                                           void* preferredRuntimeModelPtr,
                                           ModelInstanceRecord& out,
                                           uint32_t& outOffset,
                                           void*& outCandidatePtr) {
  out = {};
  outOffset = 0u;
  outCandidatePtr = nullptr;
  if (sourceObjectPtr == nullptr)
    return false;
  if (!IsReadableRange(reinterpret_cast<const uint8_t*>(sourceObjectPtr),
                       sizeof(void*))) {
    return false;
  }

  return TryResolveAttachmentIdentityFromBlock(
      sourceObjectPtr, 0x100u, preferredRuntimeModelPtr,
      out, outOffset, outCandidatePtr);
}

void RecordSpriteFrameSourceObjectFieldProbe(void* sourceObjectPtr,
                                             void* preferredRuntimeModelPtr) {
  if (sourceObjectPtr == nullptr)
    return;
  if (!IsReadableRange(reinterpret_cast<const uint8_t*>(sourceObjectPtr),
                       sizeof(void*))) {
    return;
  }

  g_lastSpriteFrameSourceObjectVtablePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(TryReadPtrFast(sourceObjectPtr, 0u))),
      std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectSceneNodeCandidatePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(TryReadPtrFast(sourceObjectPtr, 0x20u))),
      std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectSpriteCandidatePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(TryReadPtrFast(sourceObjectPtr, 0x28u))),
      std::memory_order_relaxed);

  constexpr uint32_t kScanLimit = 0x100u;
  constexpr uint32_t kPointerStride = sizeof(void*);
  for (uint32_t offset = 0u; offset <= kScanLimit; offset += kPointerStride) {
    void* candidatePtr = TryReadPtrFast(sourceObjectPtr, offset);
    if (candidatePtr == nullptr || candidatePtr == sourceObjectPtr)
      continue;

    if (LooksLikeRuntimeModelPtr(candidatePtr)) {
      g_spriteFrameSourceObjectRuntimeFieldCandidateCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(candidatePtr)),
          std::memory_order_relaxed);
      g_lastSpriteFrameSourceObjectRuntimeFieldOffset.store(
          offset, std::memory_order_relaxed);
    }

    ModelInstanceRecord candidateRecord = {};
    if (TryResolveIdentityFromPointerCandidate(candidatePtr,
                                              preferredRuntimeModelPtr,
                                              candidateRecord)) {
      g_spriteFrameSourceObjectRegistryFieldHitCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSpriteFrameSourceObjectRegistryFieldCandidatePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(candidatePtr)),
          std::memory_order_relaxed);
      g_lastSpriteFrameSourceObjectRegistryFieldOffset.store(
          offset, std::memory_order_relaxed);
    }
  }
}

bool TryResolveAttachmentIdentityFromSpriteParentHints(
    const std::array<void*, 5u>& candidates, void* preferredRuntimeModelPtr,
    ModelInstanceRecord& out, void*& outSpritePtr) {
  out = {};
  outSpritePtr = nullptr;

  const auto trySpriteHint = [&](void* spritePtr,
                                 void* currentRuntimeModelPtr) -> bool {
    if (spritePtr == nullptr)
      return false;

    ModelInstanceRecord hintRecord = {};
    if (!TryResolveParentSpriteOwnerHint(spritePtr, currentRuntimeModelPtr,
                                         hintRecord)) {
      return false;
    }

    MergeAttachmentIdentityFromInstance(out, hintRecord);
    if (out.spritePtr == nullptr)
      out.spritePtr = spritePtr;
    if (out.runtimeModelPtr == nullptr)
      out.runtimeModelPtr = currentRuntimeModelPtr;
    outSpritePtr = spritePtr;
    return HasAttachmentIdentity(out);
  };

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;

    ModelInstanceRecord instanceRecord = {};
    if (!instanceRegistry.findByRuntimeModel(candidate, instanceRecord) ||
        instanceRecord.spritePtr == nullptr) {
      continue;
    }

    void* currentRuntimeModelPtr =
        preferredRuntimeModelPtr != nullptr ? preferredRuntimeModelPtr
                                            : candidate;
    if (trySpriteHint(instanceRecord.spritePtr, currentRuntimeModelPtr))
      return true;
  }

  auto& poseRegistry = PoseRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;

    PoseRecord poseRecord = {};
    if (!poseRegistry.findByRuntimeModel(candidate, poseRecord) ||
        poseRecord.spritePtr == nullptr) {
      continue;
    }

    void* currentRuntimeModelPtr =
        preferredRuntimeModelPtr != nullptr ? preferredRuntimeModelPtr
                                            : candidate;
    if (trySpriteHint(poseRecord.spritePtr, currentRuntimeModelPtr))
      return true;
  }

  return HasAttachmentIdentity(out);
}

bool TryFindSpriteBoundAttachmentCandidate(
    const std::array<void*, 5u>& candidates, void*& outRuntimeModelPtr,
    void*& outSpritePtr) {
  outRuntimeModelPtr = nullptr;
  outSpritePtr = nullptr;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;

    ModelInstanceRecord instanceRecord = {};
    if (!instanceRegistry.findByRuntimeModel(candidate, instanceRecord) ||
        instanceRecord.spritePtr == nullptr) {
      continue;
    }

    outRuntimeModelPtr = candidate;
    outSpritePtr = instanceRecord.spritePtr;
    return true;
  }

  auto& poseRegistry = PoseRegistry::instance();
  for (void* candidate : candidates) {
    if (candidate == nullptr)
      continue;

    PoseRecord poseRecord = {};
    if (!poseRegistry.findByRuntimeModel(candidate, poseRecord) ||
        poseRecord.spritePtr == nullptr) {
      continue;
    }

    outRuntimeModelPtr = candidate;
    outSpritePtr = poseRecord.spritePtr;
    return true;
  }

  return false;
}

void NoteAttachmentRigidRecord(void* rootRuntimeModelPtr,
                               void* ownerRuntimeModelPtr,
                               void* childRuntimeModelPtr,
                               void* childSpritePtr,
                               const ModelInstanceRecord* identityRecord,
                               uint32_t slotIndex,
                               uint32_t sourceRecordIndex,
                               uint32_t childTag,
                               float localPointX,
                               float localPointY,
                               float localPointZ) {
  if constexpr (!dxvk::war3::internal::kShadowAttachmentRigidContractEnabled)
    return;
  if (rootRuntimeModelPtr == nullptr || childRuntimeModelPtr == nullptr)
    return;

  ModelInstanceRecord ownerRecord = {};
  if (identityRecord != nullptr) {
    ownerRecord = *identityRecord;
  }
  if (ownerRecord.runtimeModelPtr == nullptr)
    ownerRecord.runtimeModelPtr = childRuntimeModelPtr;
  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto mergeRuntimeSource = [&](void* runtimeModelPtr,
                                std::atomic<uint64_t>& counter) {
    if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
      return;
    const bool hadSourceObject =
        ownerRecord.sourceObjectPtr != nullptr ||
        ownerRecord.sourceSpriteObjectPtr != nullptr;
    const bool hadStrongIdentity = HasAttachmentIdentity(ownerRecord);
    ModelInstanceRecord runtimeRecord = {};
    if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, runtimeRecord))
      MergeAttachmentIdentityFromInstance(ownerRecord, runtimeRecord);
    if (instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr, runtimeRecord))
      MergeAttachmentIdentityFromInstance(ownerRecord, runtimeRecord);
    bool resolvedStrongIdentity = false;
    uint32_t ancestorDepth = 0u;
    void* ancestorRuntimeModelPtr = nullptr;
    MergeAttachmentHintsFromAncestorRuntimes(runtimeModelPtr, ownerRecord,
                                             resolvedStrongIdentity,
                                             ancestorDepth,
                                             ancestorRuntimeModelPtr);
    const bool hasSourceObject =
        ownerRecord.sourceObjectPtr != nullptr ||
        ownerRecord.sourceSpriteObjectPtr != nullptr;
    if (!hadSourceObject && hasSourceObject)
      counter.fetch_add(1u, std::memory_order_relaxed);
    if (!hadStrongIdentity && resolvedStrongIdentity) {
      g_attachmentAncestorIdentityHintWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastAttachmentAncestorFromRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
          std::memory_order_relaxed);
      g_lastAttachmentAncestorRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(ancestorRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastAttachmentAncestorDepth.store(ancestorDepth,
                                          std::memory_order_relaxed);
    }
  };
  mergeRuntimeSource(childRuntimeModelPtr,
                     g_attachmentRigidSourceObjectFromChildRuntimeCount);
  mergeRuntimeSource(ownerRuntimeModelPtr,
                     g_attachmentRigidSourceObjectFromOwnerRuntimeCount);
  mergeRuntimeSource(rootRuntimeModelPtr,
                     g_attachmentRigidSourceObjectFromRootRuntimeCount);
  if (!HasAttachmentIdentity(ownerRecord) &&
      ownerRecord.sourceObjectPtr != nullptr) {
    void* unitPtr = nullptr;
    uint32_t jHandle = 0u;
    uint32_t rawcode = 0u;
    render::ObjectKind sourceKind = render::ObjectKind::Unknown;
    if (TryResolveSourceObjectIdentity(ownerRecord.sourceObjectPtr,
                                       ownerRecord.spritePtr, unitPtr, jHandle,
                                       rawcode, sourceKind)) {
      ownerRecord.unitPtr = unitPtr;
      ownerRecord.jHandle = jHandle;
      ownerRecord.rawcode = rawcode;
    }
  }
  render::ObjectKind ownerKind = render::ObjectKind::Unknown;
  TryResolveCurrentRenderOwnerHint(nullptr, childRuntimeModelPtr, ownerRecord,
                                   ownerKind);
  if (ownerRecord.sourceObjectPtr != nullptr ||
      ownerRecord.sourceSpriteObjectPtr != nullptr) {
    g_attachmentRigidPublishedWithSourceObjectCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastAttachmentRigidSourceObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.sourceObjectPtr)),
        std::memory_order_relaxed);
    g_lastAttachmentRigidSourceSpriteObjectPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.sourceSpriteObjectPtr)),
        std::memory_order_relaxed);
  }

  if (HasAttachmentIdentity(ownerRecord)) {
    instanceRegistry.noteRuntimeOwnerIdentity(
        childRuntimeModelPtr, ownerRecord.worldObjectEntry,
        ownerRecord.sceneNode, ownerRecord.unitPtr, ownerRecord.spritePtr,
        ownerRecord.jHandle, ownerRecord.rawcode);
    if (ownerRuntimeModelPtr != nullptr) {
      instanceRegistry.noteRuntimeOwnerIdentity(
          ownerRuntimeModelPtr, ownerRecord.worldObjectEntry,
          ownerRecord.sceneNode, ownerRecord.unitPtr, ownerRecord.spritePtr,
          ownerRecord.jHandle, ownerRecord.rawcode);
    }
    instanceRegistry.noteRuntimeOwnerIdentity(
        rootRuntimeModelPtr, ownerRecord.worldObjectEntry,
        ownerRecord.sceneNode, ownerRecord.unitPtr, ownerRecord.spritePtr,
        ownerRecord.jHandle, ownerRecord.rawcode);
  }

  AttachmentRigidRegistry::instance().noteAttachmentRigid(
      rootRuntimeModelPtr, ownerRuntimeModelPtr, childRuntimeModelPtr,
      childSpritePtr,
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      ownerRecord.sourceObjectPtr, ownerRecord.sourceSpriteObjectPtr,
      ownerRecord.jHandle, ownerRecord.rawcode, slotIndex, sourceRecordIndex,
      childTag, localPointX, localPointY, localPointZ);
  g_attachmentRigidPublishedCount.fetch_add(1u, std::memory_order_relaxed);
}

uint64_t CurrentProbeFrame() {
  const uint64_t poseFrame = PoseRegistry::instance().frameNumber();
  if (poseFrame != 0u)
    return poseFrame;
  return ModelInstanceRegistry::instance().frameNumber();
}

struct OverrideOutputDedupeState {
  uint64_t frame = 0u;
  std::unordered_set<uint64_t> primary;
  std::unordered_set<uint64_t> shared;
  std::unordered_set<uint64_t> localPoint;
};

bool MarkOverrideOutputProcessedThisFrame(uint32_t kind, int contextPtr,
                                          int nodePtr) {
  const uint64_t frame = CurrentProbeFrame();
  if (frame == 0u)
    return false;

  static thread_local OverrideOutputDedupeState s_state;
  if (s_state.frame != frame) {
    s_state.frame = frame;
    s_state.primary.clear();
    s_state.shared.clear();
    s_state.localPoint.clear();
    s_state.primary.reserve(128u);
    s_state.shared.reserve(1024u);
    s_state.localPoint.reserve(1024u);
  }

  std::unordered_set<uint64_t>* set = nullptr;
  switch (kind) {
  case 0u:
    set = &s_state.primary;
    break;
  case 1u:
    set = &s_state.shared;
    break;
  default:
    set = &s_state.localPoint;
    break;
  }

  const uint64_t key =
      (uint64_t(uint32_t(contextPtr)) << 32u) | uint32_t(nodePtr);
  if (set->size() > 32768u)
    set->clear();
  return !set->insert(key).second;
}

void NoteOverrideProbeActivity(void* runtimeModelPtr) {
  const uint64_t frame = CurrentProbeFrame();
  g_overrideOutputSampleFrame.store(frame, std::memory_order_relaxed);
  g_overrideOutputLastActiveFrame.store(frame, std::memory_order_relaxed);
  g_overrideLastRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
}

void RecordOverridePrimaryPresetWrite(int contextPtr, int nodePtr) {
  if (MarkOverrideOutputProcessedThisFrame(0u, contextPtr, nodePtr))
    return;

  void* runtimeModelPtr =
      TryReadContextRuntimeModelHot(reinterpret_cast<const void*>(contextPtr));
  if (runtimeModelPtr == nullptr)
    return;

  const uint32_t slotIndex = TryReadTrustedU32(
      reinterpret_cast<const void*>(nodePtr),
      dxvk::war3::RenderStagePresetOverrideNodeOffsets::OutputSlotIndex);

  g_overridePrimaryPresetWriteCount.fetch_add(1u, std::memory_order_relaxed);
  UpdateAtomicMax(g_overrideMaxPrimaryPresetSlotIndex, slotIndex);
  if (g_config.logEnabled) {
    const auto* preset = TryReadRuntimeOverridePreset(
        runtimeModelPtr, slotIndex,
        dxvk::war3::RenderOverrideGraphOutputBundleOffsets::
            PrimaryPresetOutputs);
    if (preset != nullptr) {
      g_overrideLastPrimaryPresetHash.store(
          HashBytes(preset->raw, sizeof(preset->raw)),
          std::memory_order_relaxed);
    }
  }
  NoteOverrideProbeActivity(runtimeModelPtr);
}

void RecordOverrideSharedPresetWrite(int contextPtr, int nodePtr) {
  if (MarkOverrideOutputProcessedThisFrame(1u, contextPtr, nodePtr))
    return;

  void* runtimeModelPtr =
      TryReadContextRuntimeModelHot(reinterpret_cast<const void*>(contextPtr));
  if (runtimeModelPtr == nullptr)
    return;

  const uint32_t slotIndex = TryReadTrustedU32(
      reinterpret_cast<const void*>(nodePtr),
      dxvk::war3::RenderStagePresetOverrideNodeOffsets::OutputSlotIndex);

  g_overrideSharedPresetWriteCount.fetch_add(1u, std::memory_order_relaxed);
  UpdateAtomicMax(g_overrideMaxSharedPresetSlotIndex, slotIndex);
  if (g_config.logEnabled) {
    const auto* preset = TryReadRuntimeOverridePreset(
        runtimeModelPtr, slotIndex,
        dxvk::war3::RenderOverrideGraphOutputBundleOffsets::
            SharedPresetOutputs);
    if (preset != nullptr) {
      g_overrideLastSharedPresetHash.store(
          HashBytes(preset->raw, sizeof(preset->raw)),
          std::memory_order_relaxed);
    }
  }
  NoteOverrideProbeActivity(runtimeModelPtr);
}

void RecordOverrideLocalPointWrite(int contextPtr, int nodePtr) {
  if (MarkOverrideOutputProcessedThisFrame(2u, contextPtr, nodePtr))
    return;

  const void* contextPtrValue = reinterpret_cast<const void*>(contextPtr);
  void* runtimeModelPtr =
      TryReadContextRuntimeModelHot(contextPtrValue);
  if (runtimeModelPtr == nullptr)
    return;

  const uint32_t slotIndex = TryReadTrustedU32(
      reinterpret_cast<const void*>(nodePtr),
      dxvk::war3::RenderStagePresetOverrideNodeOffsets::OutputSlotIndex);
  const uint32_t sourceRecordIndex = TryReadTrustedU32(
      reinterpret_cast<const void*>(nodePtr),
      dxvk::war3::RenderStagePresetOverrideNodeOffsets::SourceRecordIndex);
  const auto* output =
      TryReadRuntimeLocalPointOutputHot(runtimeModelPtr, slotIndex);
  if (output == nullptr)
    return;

  const float x = output->resolved_local_point[0];
  const float y = output->resolved_local_point[1];
  const float z = output->resolved_local_point[2];
  const bool nonZero =
      (x < -1.0e-5f || x > 1.0e-5f) ||
      (y < -1.0e-5f || y > 1.0e-5f) ||
      (z < -1.0e-5f || z > 1.0e-5f);

  g_overrideLocalPointWriteCount.fetch_add(1u, std::memory_order_relaxed);
  if (nonZero)
    g_overrideLocalPointNonZeroWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
  UpdateAtomicMax(g_overrideMaxLocalPointSlotIndex, slotIndex);
  g_overrideLastLocalPointSlotIndex.store(slotIndex,
                                          std::memory_order_relaxed);
  g_overrideLastLocalPointSourceRecordIndex.store(
      sourceRecordIndex, std::memory_order_relaxed);
  g_overrideLastLocalPointXBits.store(FloatBits(x),
                                      std::memory_order_relaxed);
  g_overrideLastLocalPointYBits.store(FloatBits(y),
                                      std::memory_order_relaxed);
  g_overrideLastLocalPointZBits.store(FloatBits(z),
                                      std::memory_order_relaxed);

  if (!dxvk::war3::internal::
          kWar3RuntimeConfigSemanticAttachmentHeavyProbeEnabled) {
    void* rootRuntimeModelPtr =
        TryResolveRootRuntimeModelFromArgBlock(contextPtrValue);
    g_overrideLastRootRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(rootRuntimeModelPtr)),
        std::memory_order_relaxed);

    if (rootRuntimeModelPtr != nullptr) {
      g_overrideLocalPointRootRuntimeHitWriteCount.fetch_add(
          1u, std::memory_order_relaxed);

      RuntimeChildLinkProbeRecord chosenChildLink = {};
      uint32_t rootLinkCount = 0u;
      uint32_t rootMaxTag = 0u;
      if (TryFindDirectChildRuntimeLinkByTag(rootRuntimeModelPtr, slotIndex,
                                             sourceRecordIndex,
                                             chosenChildLink, rootLinkCount,
                                             rootMaxTag)) {
        g_overrideLocalPointRootRuntimeWithChildLinksWriteCount.fetch_add(
            1u, std::memory_order_relaxed);
        g_overrideLastRootRuntimeChildLinkCount.store(
            rootLinkCount, std::memory_order_relaxed);
        g_overrideLastRootRuntimeMaxTag.store(rootMaxTag,
                                              std::memory_order_relaxed);
        if (chosenChildLink.tag == slotIndex) {
          g_overrideLocalPointRootRuntimeMatchedChildLinkWriteCount.fetch_add(
              1u, std::memory_order_relaxed);
        } else if (chosenChildLink.tag == sourceRecordIndex) {
          g_overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount
              .fetch_add(1u, std::memory_order_relaxed);
        }

        const uint32_t childMatrixCount =
            TryReadRuntimeMatrixCountFast(chosenChildLink.childRuntimeModelPtr);
        if (childMatrixCount != 0u) {
          if (chosenChildLink.tag == slotIndex) {
            g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount
                .fetch_add(1u, std::memory_order_relaxed);
          } else if (chosenChildLink.tag == sourceRecordIndex) {
            g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount
                .fetch_add(1u, std::memory_order_relaxed);
          }
        }
        g_overrideLastMatchedChildRuntimeModelPtr.store(
            uint64_t(reinterpret_cast<uintptr_t>(
                chosenChildLink.childRuntimeModelPtr)),
            std::memory_order_relaxed);

        void* ownerRuntimeModelPtr = ChooseAttachmentOwnerRuntimeModel(
            rootRuntimeModelPtr, runtimeModelPtr, nullptr, nullptr);
        RecordObservedRuntimeChildLink(rootRuntimeModelPtr,
                                       chosenChildLink.childRuntimeModelPtr,
                                       chosenChildLink.sourceMeta,
                                       chosenChildLink.bucketIndex);
        NoteAttachmentRigidRecord(
            rootRuntimeModelPtr, ownerRuntimeModelPtr,
            chosenChildLink.childRuntimeModelPtr, nullptr, nullptr, slotIndex,
            sourceRecordIndex, chosenChildLink.tag, x, y, z);
      } else {
        g_overrideLastRootRuntimeChildLinkCount.store(
            rootLinkCount, std::memory_order_relaxed);
        g_overrideLastRootRuntimeMaxTag.store(rootMaxTag,
                                              std::memory_order_relaxed);
      }
    } else {
      g_overrideLastRootRuntimeChildLinkCount.store(
          0u, std::memory_order_relaxed);
      g_overrideLastRootRuntimeMaxTag.store(0u, std::memory_order_relaxed);
    }
    NoteOverrideProbeActivity(runtimeModelPtr);
    return;
  }

  std::vector<RuntimeChildLinkProbeRecord> childLinks;
  CollectDirectChildRuntimeLinks(runtimeModelPtr, childLinks);
  if (!childLinks.empty())
    g_overrideLocalPointObservedChildLinkWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
  UpdateAtomicMax(g_overrideMaxObservedChildLinkCount,
                  uint32_t(childLinks.size()));
  g_overrideLastObservedChildLinkCount.store(uint32_t(childLinks.size()),
                                             std::memory_order_relaxed);
  uint32_t matchedLinkCount = 0u;
  uint32_t matchedChildMatrixCount = 0u;
  void* matchedChildRuntimeModelPtr = nullptr;
  bool matchedChildHasPalette = false;
  uint32_t matchedBySourceRecordLinkCount = 0u;
  uint32_t matchedBySourceRecordMatrixCount = 0u;
  void* matchedBySourceRecordRuntimeModelPtr = nullptr;
  bool matchedBySourceRecordHasPalette = false;
  for (const auto& childLink : childLinks) {
    UpdateAtomicMax(g_overrideMaxObservedChildLinkTag, childLink.tag);

    if (childLink.childRuntimeModelPtr != nullptr &&
        childLink.tag == slotIndex) {
      matchedLinkCount += 1u;
      const uint32_t candidateMatrixCount =
          TryReadRuntimeMatrixCountFast(childLink.childRuntimeModelPtr);
      if (candidateMatrixCount > matchedChildMatrixCount) {
        matchedChildMatrixCount = candidateMatrixCount;
        matchedChildRuntimeModelPtr = childLink.childRuntimeModelPtr;
      }
      if (candidateMatrixCount != 0u)
        matchedChildHasPalette = true;
    }

    if (childLink.childRuntimeModelPtr == nullptr ||
        childLink.tag != sourceRecordIndex) {
      continue;
    }

    matchedBySourceRecordLinkCount += 1u;
    const uint32_t candidateMatrixCount =
        TryReadRuntimeMatrixCountFast(childLink.childRuntimeModelPtr);
    if (candidateMatrixCount > matchedBySourceRecordMatrixCount) {
      matchedBySourceRecordMatrixCount = candidateMatrixCount;
      matchedBySourceRecordRuntimeModelPtr = childLink.childRuntimeModelPtr;
    }
    if (candidateMatrixCount != 0u)
      matchedBySourceRecordHasPalette = true;
  }
  if (matchedLinkCount != 0u)
    g_overrideLocalPointMatchedChildLinkWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (matchedChildHasPalette)
    g_overrideLocalPointMatchedChildPaletteReadyWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
  g_overrideLastMatchedChildLinkCount.store(matchedLinkCount,
                                            std::memory_order_relaxed);
  g_overrideLastMatchedChildMatrixCount.store(matchedChildMatrixCount,
                                              std::memory_order_relaxed);
  g_overrideLastMatchedChildRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(matchedChildRuntimeModelPtr)),
      std::memory_order_relaxed);
  if (matchedBySourceRecordLinkCount != 0u)
    g_overrideLocalPointMatchedChildLinkBySourceRecordWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
  if (matchedBySourceRecordHasPalette)
    g_overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount
        .fetch_add(1u, std::memory_order_relaxed);
  g_overrideLastMatchedChildBySourceRecordLinkCount.store(
      matchedBySourceRecordLinkCount, std::memory_order_relaxed);
  g_overrideLastMatchedChildBySourceRecordMatrixCount.store(
      matchedBySourceRecordMatrixCount, std::memory_order_relaxed);
  g_overrideLastMatchedChildBySourceRecordRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(
          matchedBySourceRecordRuntimeModelPtr)),
      std::memory_order_relaxed);

  RuntimeContextChildLinkProbeRecord contextRuntime = {};
  std::vector<RuntimeChildLinkProbeRecord> contextChildLinks;
  if (TryCollectContextRuntimeWithChildLinks(contextPtrValue, contextRuntime,
                                             contextChildLinks)) {
    g_overrideLocalPointContextRuntimeWithChildLinksWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_overrideLastContextRuntimeWithChildLinksPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(contextRuntime.runtimeModelPtr)),
        std::memory_order_relaxed);
    g_overrideLastContextRuntimeWithChildLinksOffset.store(
        contextRuntime.offset, std::memory_order_relaxed);
    g_overrideLastContextRuntimeWithChildLinksCount.store(
        uint32_t(contextChildLinks.size()), std::memory_order_relaxed);
    g_overrideLastContextRuntimeWithChildLinksMaxTag.store(
        contextRuntime.maxTag, std::memory_order_relaxed);

    uint32_t contextMatchedBySlot = 0u;
    uint32_t contextMatchedBySourceRecord = 0u;
    bool contextMatchedBySourceRecordHasPalette = false;
    for (const auto& childLink : contextChildLinks) {
      if (childLink.childRuntimeModelPtr == nullptr)
        continue;
      if (childLink.tag == slotIndex)
        contextMatchedBySlot += 1u;
      if (childLink.tag != sourceRecordIndex)
        continue;
      contextMatchedBySourceRecord += 1u;
      if (TryReadRuntimeMatrixCountFast(childLink.childRuntimeModelPtr) != 0u)
        contextMatchedBySourceRecordHasPalette = true;
    }

    if (contextMatchedBySlot != 0u)
      g_overrideLocalPointContextMatchedChildLinkWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    if (contextMatchedBySourceRecord != 0u)
      g_overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
    if (contextMatchedBySourceRecordHasPalette)
      g_overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
  }

  void* scratchRootPtr = TryReadPtrFast(contextPtrValue, 72u);
  g_overrideLastScratchRootPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(scratchRootPtr)),
      std::memory_order_relaxed);
  void* scratchRootRuntimeModelPtr =
      TryResolveScratchRootRuntimeModel(contextPtrValue);
  g_overrideLastScratchRootRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(scratchRootRuntimeModelPtr)),
      std::memory_order_relaxed);
  if (scratchRootRuntimeModelPtr != nullptr) {
    std::vector<RuntimeChildLinkProbeRecord> scratchRootChildLinks;
    CollectDirectChildRuntimeLinks(scratchRootRuntimeModelPtr,
                                   scratchRootChildLinks);
    if (!scratchRootChildLinks.empty()) {
      g_overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
    g_overrideLastScratchRootRuntimeChildLinkCount.store(
        uint32_t(scratchRootChildLinks.size()), std::memory_order_relaxed);
    uint32_t scratchRootMaxTag = 0u;
    uint32_t scratchRootMatchedBySlot = 0u;
    uint32_t scratchRootMatchedBySourceRecord = 0u;
    bool scratchRootMatchedBySourceRecordHasPalette = false;
    for (const auto& childLink : scratchRootChildLinks) {
      scratchRootMaxTag = std::max(scratchRootMaxTag, childLink.tag);
      if (childLink.childRuntimeModelPtr == nullptr)
        continue;
      if (childLink.tag == slotIndex)
        scratchRootMatchedBySlot += 1u;
      if (childLink.tag != sourceRecordIndex)
        continue;
      scratchRootMatchedBySourceRecord += 1u;
      if (TryReadRuntimeMatrixCountFast(childLink.childRuntimeModelPtr) != 0u)
        scratchRootMatchedBySourceRecordHasPalette = true;
    }
    g_overrideLastScratchRootRuntimeMaxTag.store(
        scratchRootMaxTag, std::memory_order_relaxed);
    if (scratchRootMatchedBySlot != 0u)
      g_overrideLocalPointScratchRootMatchedChildLinkWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    if (scratchRootMatchedBySourceRecord != 0u)
      g_overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
    if (scratchRootMatchedBySourceRecordHasPalette)
      g_overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
  } else {
    g_overrideLastScratchRootRuntimeChildLinkCount.store(
        0u, std::memory_order_relaxed);
    g_overrideLastScratchRootRuntimeMaxTag.store(
        0u, std::memory_order_relaxed);
  }

  void* argBlockPtr = TryReadPtrFast(contextPtrValue, 72u);
  g_overrideLastArgBlockPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(argBlockPtr)),
      std::memory_order_relaxed);
  g_overrideLastArgBlockIdentityHintPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastArgBlockIdentityHintOffset.store(0u,
                                                 std::memory_order_relaxed);
  RuntimeContextChildLinkProbeRecord argBlockRuntime = {};
  std::vector<RuntimeChildLinkProbeRecord> argBlockChildLinks;
  if (TryCollectRuntimeWithChildLinksFromBlock(argBlockPtr, 0x80u,
                                               argBlockRuntime,
                                               argBlockChildLinks)) {
    g_overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(argBlockRuntime.runtimeModelPtr)),
        std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeOffset.store(
        argBlockRuntime.offset, std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeChildLinkCount.store(
        uint32_t(argBlockChildLinks.size()), std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeMaxTag.store(
        argBlockRuntime.maxTag, std::memory_order_relaxed);

    uint32_t argBlockMatchedBySlot = 0u;
    uint32_t argBlockMatchedBySourceRecord = 0u;
    for (const auto& childLink : argBlockChildLinks) {
      if (childLink.childRuntimeModelPtr == nullptr)
        continue;
      if (childLink.tag == slotIndex)
        argBlockMatchedBySlot += 1u;
      if (childLink.tag == sourceRecordIndex)
        argBlockMatchedBySourceRecord += 1u;
    }
    if (argBlockMatchedBySlot != 0u)
      g_overrideLocalPointArgBlockMatchedChildLinkWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    if (argBlockMatchedBySourceRecord != 0u)
      g_overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
  } else {
    g_overrideLastArgBlockRuntimeModelPtr.store(
        0u, std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeOffset.store(0u, std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeChildLinkCount.store(
        0u, std::memory_order_relaxed);
    g_overrideLastArgBlockRuntimeMaxTag.store(0u, std::memory_order_relaxed);
  }

  void* arg4BlockPtr = TryReadPtrFast(contextPtrValue, 68u);
  g_overrideLastArg4BlockPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(arg4BlockPtr)),
      std::memory_order_relaxed);
  g_overrideLastArg4BlockIdentityHintPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastArg4BlockIdentityHintOffset.store(0u,
                                                  std::memory_order_relaxed);
  g_overrideLastSpriteBoundCandidateSpritePtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastSpriteBoundCandidateRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastParentSpriteIdentityHintSpritePtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastParentSpriteIdentityHintRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  RuntimeContextChildLinkProbeRecord arg4BlockRuntime = {};
  std::vector<RuntimeChildLinkProbeRecord> arg4BlockChildLinks;
  if (TryCollectRuntimeWithChildLinksFromBlock(arg4BlockPtr, 0x80u,
                                               arg4BlockRuntime,
                                               arg4BlockChildLinks)) {
    g_overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(arg4BlockRuntime.runtimeModelPtr)),
        std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeOffset.store(
        arg4BlockRuntime.offset, std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeChildLinkCount.store(
        uint32_t(arg4BlockChildLinks.size()), std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeMaxTag.store(
        arg4BlockRuntime.maxTag, std::memory_order_relaxed);

    uint32_t arg4BlockMatchedBySlot = 0u;
    uint32_t arg4BlockMatchedBySourceRecord = 0u;
    for (const auto& childLink : arg4BlockChildLinks) {
      if (childLink.childRuntimeModelPtr == nullptr)
        continue;
      if (childLink.tag == slotIndex)
        arg4BlockMatchedBySlot += 1u;
      if (childLink.tag == sourceRecordIndex)
        arg4BlockMatchedBySourceRecord += 1u;
    }
    if (arg4BlockMatchedBySlot != 0u)
      g_overrideLocalPointArg4BlockMatchedChildLinkWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    if (arg4BlockMatchedBySourceRecord != 0u)
      g_overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
  } else {
    g_overrideLastArg4BlockRuntimeModelPtr.store(
        0u, std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeOffset.store(0u, std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeChildLinkCount.store(
        0u, std::memory_order_relaxed);
    g_overrideLastArg4BlockRuntimeMaxTag.store(0u, std::memory_order_relaxed);
  }

  void* rootRuntimeModelPtr = TryResolveRootRuntimeModelFromArgBlock(
      contextPtrValue);
  g_overrideLastRootRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(rootRuntimeModelPtr)),
      std::memory_order_relaxed);
  if (rootRuntimeModelPtr != nullptr) {
    g_overrideLocalPointRootRuntimeHitWriteCount.fetch_add(
        1u, std::memory_order_relaxed);

    std::vector<RuntimeChildLinkProbeRecord> rootChildLinks;
    CollectDirectChildRuntimeLinks(rootRuntimeModelPtr, rootChildLinks);
    if (!rootChildLinks.empty()) {
      g_overrideLocalPointRootRuntimeWithChildLinksWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    }

    g_overrideLastRootRuntimeChildLinkCount.store(
        uint32_t(rootChildLinks.size()), std::memory_order_relaxed);
    uint32_t rootMaxTag = 0u;
    uint32_t rootMatchedBySlot = 0u;
    uint32_t rootMatchedBySourceRecord = 0u;
    uint32_t rootMatchedChildMatrixCount = 0u;
    uint32_t rootMatchedBySourceRecordMatrixCount = 0u;
    bool rootMatchedBySlotHasPalette = false;
    bool rootMatchedBySourceRecordHasPalette = false;
    RuntimeChildLinkProbeRecord slotMatch = {};
    RuntimeChildLinkProbeRecord sourceMatch = {};
    for (const auto& childLink : rootChildLinks) {
      rootMaxTag = (std::max)(rootMaxTag, childLink.tag);
      if (childLink.childRuntimeModelPtr == nullptr)
        continue;

      const uint32_t candidateMatrixCount =
          TryReadRuntimeMatrixCountFast(childLink.childRuntimeModelPtr);
      if (childLink.tag == slotIndex) {
        rootMatchedBySlot += 1u;
        if (candidateMatrixCount > rootMatchedChildMatrixCount) {
          rootMatchedChildMatrixCount = candidateMatrixCount;
          slotMatch = childLink;
        }
        if (candidateMatrixCount != 0u)
          rootMatchedBySlotHasPalette = true;
      }
      if (childLink.tag == sourceRecordIndex) {
        rootMatchedBySourceRecord += 1u;
        if (candidateMatrixCount > rootMatchedBySourceRecordMatrixCount) {
          rootMatchedBySourceRecordMatrixCount = candidateMatrixCount;
          sourceMatch = childLink;
        }
        if (candidateMatrixCount != 0u)
          rootMatchedBySourceRecordHasPalette = true;
      }
    }

    g_overrideLastRootRuntimeMaxTag.store(rootMaxTag,
                                          std::memory_order_relaxed);
    if (rootMatchedBySlot != 0u)
      g_overrideLocalPointRootRuntimeMatchedChildLinkWriteCount.fetch_add(
          1u, std::memory_order_relaxed);
    if (rootMatchedBySlotHasPalette)
      g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
    if (rootMatchedBySourceRecord != 0u)
      g_overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);
    if (rootMatchedBySourceRecordHasPalette)
      g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount
          .fetch_add(1u, std::memory_order_relaxed);

    const bool allowSourceRecordFallback =
        dxvk::war3::internal::kShadowAttachmentRigidAllowSourceRecordKeyFallback;
    RuntimeChildLinkProbeRecord chosenChildLink = {};
    if (slotMatch.childRuntimeModelPtr != nullptr) {
      chosenChildLink = slotMatch;
    } else if (allowSourceRecordFallback &&
               sourceMatch.childRuntimeModelPtr != nullptr) {
      chosenChildLink = sourceMatch;
    }

    if (chosenChildLink.childRuntimeModelPtr != nullptr) {
      ModelInstanceRecord identityRecord = {};
      const std::array<void*, 5u> identityCandidates = {
          rootRuntimeModelPtr,
          chosenChildLink.childRuntimeModelPtr,
          runtimeModelPtr,
          argBlockRuntime.runtimeModelPtr,
          arg4BlockRuntime.runtimeModelPtr,
      };
      void* spriteBoundCandidateRuntimeModelPtr = nullptr;
      void* spriteBoundCandidateSpritePtr = nullptr;
      if (TryFindSpriteBoundAttachmentCandidate(
              identityCandidates, spriteBoundCandidateRuntimeModelPtr,
              spriteBoundCandidateSpritePtr)) {
        g_overrideLocalPointSpriteBoundCandidateWriteCount.fetch_add(
            1u, std::memory_order_relaxed);
        g_overrideLastSpriteBoundCandidateSpritePtr.store(
            uint64_t(reinterpret_cast<uintptr_t>(
                spriteBoundCandidateSpritePtr)),
            std::memory_order_relaxed);
        g_overrideLastSpriteBoundCandidateRuntimeModelPtr.store(
            uint64_t(reinterpret_cast<uintptr_t>(
                spriteBoundCandidateRuntimeModelPtr)),
            std::memory_order_relaxed);
      }
      void* ownerRuntimeModelPtr = ChooseAttachmentOwnerRuntimeModel(
          rootRuntimeModelPtr, runtimeModelPtr, argBlockRuntime.runtimeModelPtr,
          arg4BlockRuntime.runtimeModelPtr);
      void* attachmentChildSpritePtr = nullptr;
      if (g_attachModelToPointScopeState.depth != 0u &&
          g_attachModelToPointScopeState.childSpritePtr != nullptr) {
        const void* scopeParentRuntimeModelPtr =
            g_attachModelToPointScopeState.parentRuntimeModelPtr;
        if (scopeParentRuntimeModelPtr == nullptr ||
            scopeParentRuntimeModelPtr == ownerRuntimeModelPtr ||
            scopeParentRuntimeModelPtr == rootRuntimeModelPtr ||
            scopeParentRuntimeModelPtr == runtimeModelPtr) {
          attachmentChildSpritePtr = g_attachModelToPointScopeState.childSpritePtr;
        }
      }
      if (attachmentChildSpritePtr == nullptr &&
          spriteBoundCandidateRuntimeModelPtr ==
              chosenChildLink.childRuntimeModelPtr) {
        attachmentChildSpritePtr = spriteBoundCandidateSpritePtr;
      }
      bool identityHit = TryResolveAttachmentIdentity(
          rootRuntimeModelPtr, chosenChildLink.childRuntimeModelPtr,
          runtimeModelPtr, argBlockRuntime.runtimeModelPtr,
          arg4BlockRuntime.runtimeModelPtr, identityRecord);
      if (!identityHit) {
        uint32_t identityHintOffset = 0u;
        void* identityHintPtr = nullptr;
        if (TryResolveAttachmentIdentityFromBlock(
                argBlockPtr, 0x40u, chosenChildLink.childRuntimeModelPtr,
                identityRecord, identityHintOffset, identityHintPtr)) {
          identityHit = true;
          g_overrideLocalPointArgBlockIdentityHintWriteCount.fetch_add(
              1u, std::memory_order_relaxed);
          g_overrideLastArgBlockIdentityHintPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(identityHintPtr)),
              std::memory_order_relaxed);
          g_overrideLastArgBlockIdentityHintOffset.store(
              identityHintOffset, std::memory_order_relaxed);
        }
      }
      if (!identityHit) {
        uint32_t identityHintOffset = 0u;
        void* identityHintPtr = nullptr;
        if (TryResolveAttachmentIdentityFromBlock(
                arg4BlockPtr, 0x20u, chosenChildLink.childRuntimeModelPtr,
                identityRecord, identityHintOffset, identityHintPtr)) {
          identityHit = true;
          g_overrideLocalPointArg4BlockIdentityHintWriteCount.fetch_add(
              1u, std::memory_order_relaxed);
          g_overrideLastArg4BlockIdentityHintPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(identityHintPtr)),
              std::memory_order_relaxed);
          g_overrideLastArg4BlockIdentityHintOffset.store(
              identityHintOffset, std::memory_order_relaxed);
        }
      }
      if (!identityHit) {
        void* sourceMetaPtr =
            reinterpret_cast<void*>(uintptr_t(chosenChildLink.sourceMeta));
        void* unitPtr = nullptr;
        uint32_t jHandle = 0u;
        uint32_t rawcode = 0u;
        render::ObjectKind sourceKind = render::ObjectKind::Unknown;
        if (chosenChildLink.sourceMeta != 0u && sourceMetaPtr != nullptr &&
            IsReadableRange(reinterpret_cast<const uint8_t*>(sourceMetaPtr),
                            sizeof(void*)) &&
            TryResolveSourceObjectIdentity(sourceMetaPtr, identityRecord.spritePtr,
                                           unitPtr, jHandle, rawcode,
                                           sourceKind)) {
          identityHit = true;
          identityRecord.sourceObjectPtr = sourceMetaPtr;
          identityRecord.sourceSpriteObjectPtr =
              TryReadSourceSpriteObjectPtr(sourceMetaPtr);
          identityRecord.unitPtr = unitPtr;
          identityRecord.jHandle = jHandle;
          identityRecord.rawcode = rawcode;
          g_overrideLocalPointChildSourceMetaIdentityHintWriteCount.fetch_add(
              1u, std::memory_order_relaxed);
          g_overrideLastChildSourceMetaPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(sourceMetaPtr)),
              std::memory_order_relaxed);
          g_overrideLastChildSourceMetaRuntimeModelPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(
                  chosenChildLink.childRuntimeModelPtr)),
              std::memory_order_relaxed);
        }
      }
      if (!identityHit) {
        void* parentSpriteHintPtr = nullptr;
        if (TryResolveAttachmentIdentityFromSpriteParentHints(
                identityCandidates, chosenChildLink.childRuntimeModelPtr,
                identityRecord, parentSpriteHintPtr)) {
          identityHit = true;
          g_overrideLocalPointParentSpriteIdentityHintWriteCount.fetch_add(
              1u, std::memory_order_relaxed);
          g_overrideLastParentSpriteIdentityHintSpritePtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(parentSpriteHintPtr)),
              std::memory_order_relaxed);
          g_overrideLastParentSpriteIdentityHintRuntimeModelPtr.store(
              uint64_t(reinterpret_cast<uintptr_t>(
                  chosenChildLink.childRuntimeModelPtr)),
              std::memory_order_relaxed);
        }
      }
      // local-point 路径已经现场确认了 root -> chosen child 的直接关系。
      // 如果这批 child runtime 没有走到 BuildChildRuntimeModelLinks family，
      // 这里把实时观察到的父子关系回灌到 parent-link 图，供后续 attachment
      // identity/source hint 合并使用。
      RecordObservedRuntimeChildLink(rootRuntimeModelPtr,
                                     chosenChildLink.childRuntimeModelPtr,
                                     chosenChildLink.sourceMeta,
                                     chosenChildLink.bucketIndex);
      TryBootstrapAttachmentChildRuntimeLineage(
          rootRuntimeModelPtr, ownerRuntimeModelPtr,
          chosenChildLink.childRuntimeModelPtr, chosenChildLink.sourceMeta,
          chosenChildLink.bucketIndex, &rootChildLinks);

      NoteAttachmentRigidRecord(
          rootRuntimeModelPtr, ownerRuntimeModelPtr,
          chosenChildLink.childRuntimeModelPtr, attachmentChildSpritePtr,
          identityHit ? &identityRecord : nullptr, slotIndex,
          sourceRecordIndex, chosenChildLink.tag, x, y, z);
    }
  } else {
    g_overrideLastRootRuntimeChildLinkCount.store(
        0u, std::memory_order_relaxed);
    g_overrideLastRootRuntimeMaxTag.store(0u, std::memory_order_relaxed);
  }
  NoteOverrideProbeActivity(runtimeModelPtr);
}

bool LooksLikeRuntimeModelPtr(void* candidate) {
  if (candidate == nullptr)
    return false;

  const uintptr_t candidateValue = reinterpret_cast<uintptr_t>(candidate);
  if (candidateValue < 0x10000u)
    return false;

  void* ownedHandlePtr = TryReadPtrFast(
      candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle);
  const bool hasOwnedHandle = ownedHandlePtr != nullptr;

  const uint32_t runtimeGeosetCount = TryReadU32Fast(
      candidate, dxvk::war3::CModelOffsets::RuntimeGeosetCount);
  void* runtimeGeosets = TryReadPtrFast(
      candidate, dxvk::war3::CModelOffsets::RuntimeGeosets);
  if (runtimeGeosetCount < 4096u && runtimeGeosets != nullptr) {
    return true;
  }

  const uint32_t finalPoseMatrixCount = TryReadU32Fast(
      candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount);
  void* finalPoseMatrixArray = TryReadPtrFast(
      candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray);
  const bool hasFinalPoseArray =
      finalPoseMatrixCount <= 256u && finalPoseMatrixArray != nullptr;

  return hasOwnedHandle || hasFinalPoseArray;
}

void* TryResolveDirectModelResourceFromRuntimeModel(void* runtimeModelPtr) {
  if (!runtimeModelPtr)
    return nullptr;
  void* ownedHandlePtr =
      TryReadPtrFast(runtimeModelPtr,
                     dxvk::war3::CModelOffsets::OwnedModelDataHandle);
  if (ownedHandlePtr == nullptr)
    return nullptr;

  auto& resourceCache = ShadowModelResourceCache::instance();
  return resourceCache.resolveDirectModelResourcePtr(ownedHandlePtr);
}

void* NormalizeDirectModelResourcePtr(void* modelResourcePtr) {
  if (modelResourcePtr == nullptr)
    return nullptr;
  return ShadowModelResourceCache::instance().resolveDirectModelResourcePtr(
      modelResourcePtr);
}

void* TryResolveRuntimeModelBaseAlias(void* runtimeModelPtr) {
  if (!LooksLikeRuntimeModelPtrCached(runtimeModelPtr))
    return nullptr;

  // CModelComplex hot sprite-frame observations can arrive at the +0xA0
  // extension address, while model resources/geosets are keyed by CModel base.
  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t runtimeValue = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (runtimeValue <= 0x10000u + kCModelComplexExtensionOffset)
    return nullptr;

  void* baseAliasPtr =
      reinterpret_cast<void*>(runtimeValue - kCModelComplexExtensionOffset);
  if (!LooksLikeRuntimeModelPtrCached(baseAliasPtr))
    return nullptr;

  if (NormalizeDirectModelResourcePtr(
          TryResolveDirectModelResourceFromRuntimeModel(baseAliasPtr)) != nullptr) {
    return baseAliasPtr;
  }

  ShadowModelResourceRecord runtimeResource = {};
  if (ShadowModelResourceCache::instance().findRuntimeModelResource(
          baseAliasPtr, runtimeResource) &&
      (runtimeResource.modelResourcePtr != nullptr ||
       runtimeResource.readyGeosetCount != 0u)) {
    return baseAliasPtr;
  }

  ModelResourceRecord modelRecord = {};
  if (ModelRegistry::instance().findByRuntimeModel(baseAliasPtr, modelRecord) &&
      modelRecord.modelResourcePtr != nullptr) {
    return baseAliasPtr;
  }

  return nullptr;
}

void RecordRuntimeMatrixPublisher(int runtimeModel, uint32_t publisherKind) {
  if (runtimeModel == 0)
    return;

  if (publisherKind == 1u) {
    g_runtimeMatrixRangeCopyCount.fetch_add(1u, std::memory_order_relaxed);
  } else if (publisherKind == 2u) {
    g_runtimeMatrixFlushCount.fetch_add(1u, std::memory_order_relaxed);
  }

  void* runtimeModelPtr =
      reinterpret_cast<void*>(uintptr_t(uint32_t(runtimeModel)));
  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  std::array<void*, 3> candidates = {};
  uint32_t candidateCount = 0u;
  auto addCandidate = [&](void* candidate) {
    if (candidate == nullptr)
      return;
    for (uint32_t i = 0u; i < candidateCount; ++i) {
      if (candidates[i] == candidate)
        return;
    }
    if (candidateCount < candidates.size())
      candidates[candidateCount++] = candidate;
  };

  addCandidate(runtimeModelPtr);
  const uintptr_t runtimeValue = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (runtimeValue > kCModelComplexExtensionOffset) {
    void* baseAlias =
        reinterpret_cast<void*>(runtimeValue - kCModelComplexExtensionOffset);
    if (LooksLikeRuntimeModelPtr(baseAlias))
      addCandidate(baseAlias);
  }
  if (runtimeValue <= (~uintptr_t(0u)) - kCModelComplexExtensionOffset) {
    void* extensionAlias =
        reinterpret_cast<void*>(runtimeValue + kCModelComplexExtensionOffset);
    if (LooksLikeRuntimeModelPtr(extensionAlias))
      addCandidate(extensionAlias);
  }

  auto& attachmentRegistry = AttachmentRigidRegistry::instance();
  uint32_t roleMask = 0u;
  uint32_t matrixCount = 0u;
  void* matchedRuntimeModelPtr = nullptr;
  bool aliasHit = false;

  for (uint32_t i = 0u; i < candidateCount; ++i) {
    void* candidate = candidates[i];
    const uint32_t candidateMatrixCount =
        TryReadRuntimeMatrixCountFast(candidate);
    matrixCount = std::max(matrixCount, candidateMatrixCount);

    uint32_t candidateRoleMask = 0u;
    AttachmentRigidRecord attachmentRecord = {};
    if (attachmentRegistry.findByRootRuntimeModel(candidate,
                                                  attachmentRecord)) {
      candidateRoleMask |= 0x1u;
      g_lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(candidate)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(
              attachmentRecord.ownerRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(
              attachmentRecord.childRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount.store(
          candidateMatrixCount, std::memory_order_relaxed);
      if (candidateMatrixCount != 0u) {
        g_runtimeMatrixPublisherAttachmentRootPaletteReadyCount.fetch_add(
            1u, std::memory_order_relaxed);
      }
    }
    if (attachmentRegistry.findByOwnerRuntimeModel(candidate,
                                                   attachmentRecord)) {
      candidateRoleMask |= 0x2u;
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(candidate)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(
              attachmentRecord.rootRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(
              attachmentRecord.childRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount.store(
          candidateMatrixCount, std::memory_order_relaxed);
      if (candidateMatrixCount != 0u) {
        g_runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount.fetch_add(
            1u, std::memory_order_relaxed);
      }
    }
    if (attachmentRegistry.findByChildRuntimeModel(candidate,
                                                   attachmentRecord)) {
      candidateRoleMask |= 0x4u;
      g_lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(candidate)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(
              attachmentRecord.rootRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(
              attachmentRecord.ownerRuntimeModelPtr)),
          std::memory_order_relaxed);
      g_lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount.store(
          candidateMatrixCount, std::memory_order_relaxed);
      if (candidateMatrixCount != 0u) {
        g_runtimeMatrixPublisherAttachmentChildPaletteReadyCount.fetch_add(
            1u, std::memory_order_relaxed);
      }
    }

    if (candidateRoleMask != 0u && matchedRuntimeModelPtr == nullptr)
      matchedRuntimeModelPtr = candidate;
    if (candidateRoleMask != 0u && candidate != runtimeModelPtr)
      aliasHit = true;
    roleMask |= candidateRoleMask;
  }

  if (matrixCount != 0u) {
    g_runtimeMatrixPublisherPaletteReadyCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if ((roleMask & 0x1u) != 0u) {
    g_runtimeMatrixPublisherAttachmentRootHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if ((roleMask & 0x2u) != 0u) {
    g_runtimeMatrixPublisherAttachmentOwnerHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if ((roleMask & 0x4u) != 0u) {
    g_runtimeMatrixPublisherAttachmentChildHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (aliasHit) {
    g_runtimeMatrixPublisherAttachmentAliasHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }

  g_lastRuntimeMatrixPublisherRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherMatchedRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(matchedRuntimeModelPtr)),
      std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherMatrixCount.store(matrixCount,
                                                std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherKind.store(publisherKind,
                                         std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherRoleMask.store(roleMask,
                                             std::memory_order_relaxed);
}

bool TryReadSpriteFrameTransform(void* spritePtr, Matrix4& out,
                                 float& outScale, float& outSequenceTime) {
  if (!spritePtr)
    return false;

  float raw[12] = {};
  if (!IsReadableRange(reinterpret_cast<const uint8_t*>(spritePtr) +
                           dxvk::war3::CSpriteUberOffsets::WorldMatrix3x4,
                       sizeof(raw))) {
    return false;
  }

  std::memcpy(
      raw,
      reinterpret_cast<const uint8_t*>(spritePtr) +
          dxvk::war3::CSpriteUberOffsets::WorldMatrix3x4,
      sizeof(raw));
  out = Matrix4(Vector4(raw[0], raw[1], raw[2], 0.0f),
                Vector4(raw[3], raw[4], raw[5], 0.0f),
                Vector4(raw[6], raw[7], raw[8], 0.0f),
                Vector4(raw[9], raw[10], raw[11], 1.0f));

  outScale = TryReadF32Fast(spritePtr, dxvk::war3::CSpriteUberOffsets::UniformScale);
  if (outScale == 0.0f)
    outScale = 1.0f;

  const uint32_t overrideEnabled = TryReadU32Fast(
      spritePtr, dxvk::war3::CSpriteUberOffsets::AnimationTimeOverrideEnabled);
  outSequenceTime =
      overrideEnabled != 0
          ? TryReadF32Fast(
                spritePtr,
                dxvk::war3::CSpriteUberOffsets::AnimationTimeOverrideValue)
          : 0.0f;
  return true;
}

bool TryReadRuntimeMatrixPalette(int runtimeModel, std::vector<Matrix4>& out) {
  if (runtimeModel == 0)
    return false;

  uint32_t matrixCount = 0;
  void* matrixBase = nullptr;
  if (!SafeReadU32Fast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixCountOffset, matrixCount) ||
      !SafeReadPtrFast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixArrayOffset, matrixBase) ||
      matrixBase == nullptr || matrixCount == 0) {
    return false;
  }

  matrixCount = std::min<uint32_t>(matrixCount, 256u);
  const size_t bytes = size_t(matrixCount) * 48u;
  if (!IsReadableRange(matrixBase, bytes))
    return false;

  out.resize(matrixCount);
  auto* raw = reinterpret_cast<const uint8_t*>(matrixBase);
  for (uint32_t i = 0; i < matrixCount; ++i)
    out[i] = DecodeRuntimePoseMatrix48(raw + size_t(i) * 48u);
  return true;
}

bool TryReadRuntimePoseArrayRange(int runtimeModel,
                                  RuntimePoseArrayRange& out) {
  out = {};
  if (runtimeModel == 0)
    return false;

  uint32_t matrixCount = 0u;
  void* matrixBase = nullptr;
  if (!SafeReadU32Fast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixCountOffset, matrixCount) ||
      matrixCount == 0u || matrixCount > 256u ||
      !SafeReadPtrFast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixArrayOffset, matrixBase) ||
      matrixBase == nullptr ||
      !IsReadableRange(matrixBase, size_t(matrixCount) * 48u)) {
    return false;
  }

  out.runtimeModel = uintptr_t(uint32_t(runtimeModel));
  out.matrixArray = reinterpret_cast<uintptr_t>(matrixBase);
  out.matrixCount = matrixCount;
  return true;
}

void RememberRuntimePoseArrayRange(int runtimeModel) {
  RuntimePoseArrayRange range = {};
  if (!TryReadRuntimePoseArrayRange(runtimeModel, range))
    return;

  std::unique_lock<std::shared_mutex> lock(g_runtimePoseArrayRangeMutex);
  auto previous = g_runtimePoseArrayByModel.find(range.runtimeModel);
  if (previous != g_runtimePoseArrayByModel.end()) {
    const auto old = previous->second;
    if (old.matrixArray == range.matrixArray &&
        old.matrixCount == range.matrixCount) {
      return;
    }
    for (uint32_t i = 0u; i < old.matrixCount; ++i)
      g_runtimePoseArrayByMatrixPtr.erase(old.matrixArray + size_t(i) * 48u);
  }

  g_runtimePoseArrayByModel[range.runtimeModel] = range;
  for (uint32_t i = 0u; i < range.matrixCount; ++i)
    g_runtimePoseArrayByMatrixPtr[range.matrixArray + size_t(i) * 48u] = range;

  // Phase 7.95：更新 size 原子计数，让 TryFind 的 empty-check 快路径生效。
  g_runtimePoseArrayRegistrySize.store(
      uint32_t(g_runtimePoseArrayByMatrixPtr.size()),
      std::memory_order_relaxed);
}

bool TryFindRuntimePoseArrayRangeForMatrix(int destMatrixPtr,
                                           RuntimePoseArrayRange& out,
                                           uint32_t& outMatrixIndex) {
  out = {};
  outMatrixIndex = 0u;
  if (destMatrixPtr == 0)
    return false;

  // Phase 7.95：如果 pose range registry 为空（没有注册任何 CModel pose range），
  // 直接 return false 不做 lock。在桥/斜坡场景下 War3 可能每帧 50K+ matrix write，
  // 全部 miss 但仍付 shared_lock + hashmap.find 成本。这个 relaxed load 几乎零成本。
  if (g_runtimePoseArrayRegistrySize.load(std::memory_order_relaxed) == 0u)
    return false;

  const uintptr_t dest = uintptr_t(uint32_t(destMatrixPtr));
  thread_local RuntimePoseArrayRange s_lastRange = {};
  if (s_lastRange.runtimeModel != 0u && s_lastRange.matrixArray != 0u &&
      dest >= s_lastRange.matrixArray &&
      dest < s_lastRange.matrixArray + size_t(s_lastRange.matrixCount) * 48u &&
      ((dest - s_lastRange.matrixArray) % 48u) == 0u) {
    out = s_lastRange;
    outMatrixIndex = uint32_t((dest - s_lastRange.matrixArray) / 48u);
    return true;
  }

  // Phase 7.79：reader 走 shared_lock，让 hook 高频 hot path 不再串行化。
  std::shared_lock<std::shared_mutex> lock(g_runtimePoseArrayRangeMutex);
  auto it = g_runtimePoseArrayByMatrixPtr.find(dest);
  if (it == g_runtimePoseArrayByMatrixPtr.end())
    return false;

  out = it->second;
  s_lastRange = out;
  outMatrixIndex = uint32_t((dest - out.matrixArray) / 48u);
  return true;
}

bool TryReadRuntimeMatrixPaletteFromRangeCopy(int runtimeModel, int contextPtr,
                                              int sourceMatrixBasePtr,
                                              std::vector<Matrix4>& out) {
  out.clear();
  if (runtimeModel == 0 || contextPtr == 0 || sourceMatrixBasePtr == 0)
    return false;

  uint32_t matrixCount = 0u;
  if (!SafeReadU32Fast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixCountOffset, matrixCount) ||
      matrixCount == 0u) {
    return false;
  }
  matrixCount = std::min<uint32_t>(matrixCount, 256u);

  uint32_t sourceCursor = 0u;
  uint32_t sourceBaseIndex = 0u;
  const auto* context = reinterpret_cast<const void*>(contextPtr);
  if (!SafeReadU32Fast(context, 0x54u, sourceCursor) ||
      !SafeReadU32Fast(context, 0x6Cu, sourceBaseIndex)) {
    return false;
  }

  const int64_t matrixOffset =
      int64_t(int32_t(sourceCursor)) - int64_t(int32_t(sourceBaseIndex));
  // The 0x6F12FDC0 copy path indexes 48-byte matrices. Extremely large or
  // negative offsets indicate this is not the expected animation source block.
  if (matrixOffset < 0 || matrixOffset > 65535)
    return false;

  const uintptr_t sourceBase = reinterpret_cast<uintptr_t>(
      reinterpret_cast<const void*>(sourceMatrixBasePtr));
  const uintptr_t byteOffset = uintptr_t(matrixOffset) * 48u;
  if (sourceBase < 0x10000u || sourceBase + byteOffset < sourceBase)
    return false;

  const auto* raw = reinterpret_cast<const uint8_t*>(sourceBase + byteOffset);
  const size_t bytes = size_t(matrixCount) * 48u;
  if (!IsReadableRange(raw, bytes))
    return false;

  out.resize(matrixCount);
  for (uint32_t i = 0u; i < matrixCount; ++i)
    out[i] = DecodeRuntimePoseMatrix48(raw + size_t(i) * 48u);
  return true;
}

bool TryReadRuntimeWorldTransform(int runtimeModel, Matrix4& out) {
  if (runtimeModel == 0)
    return false;

  const auto* worldBytes = reinterpret_cast<const uint8_t*>(
      reinterpret_cast<void*>(runtimeModel)) +
      dxvk::war3::CModelOffsets::WorldMatrix3x4;
  if (!IsReadableRange(worldBytes, 48u))
    return false;

  out = DecodeRuntimePoseMatrix48(worldBytes);
  return true;
}

std::vector<void*> CollectRuntimeModelTree(void* rootRuntimeModelPtr) {
  std::vector<void*> out;
  if (rootRuntimeModelPtr == nullptr)
    return out;

  std::vector<void*> pending;
  pending.reserve(32u);
  pending.push_back(rootRuntimeModelPtr);

  std::unordered_set<void*> visitedRuntimeModels;
  visitedRuntimeModels.reserve(64u);
  std::unordered_set<void*> visitedLinkNodes;
  visitedLinkNodes.reserve(128u);

  size_t cursor = 0u;
  constexpr size_t kMaxRuntimeModels = 256u;
  constexpr size_t kMaxLinkNodes = 1024u;
  while (cursor < pending.size() && pending.size() <= kMaxRuntimeModels) {
    void* currentRuntimeModel = pending[cursor++];
    if (currentRuntimeModel == nullptr ||
        !visitedRuntimeModels.insert(currentRuntimeModel).second) {
      continue;
    }
    out.push_back(currentRuntimeModel);

    uint32_t childGroupCount = 0u;
    void* childGroupArray = nullptr;
    if (!SafeReadU32Fast(currentRuntimeModel,
                         dxvk::war3::CModelOffsets::ChildBucketCount,
                         childGroupCount) ||
        !SafeReadPtrFast(currentRuntimeModel,
                         dxvk::war3::CModelOffsets::ChildBucketArray,
                         childGroupArray) ||
        childGroupCount == 0u || childGroupArray == nullptr ||
        !IsReadableRange(childGroupArray, size_t(childGroupCount) * 12u)) {
      continue;
    }

    const auto* childGroups =
        reinterpret_cast<const uint8_t*>(childGroupArray);
    for (uint32_t i = 0u; i < childGroupCount; ++i) {
      void* linkNode = nullptr;
      SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode);
      size_t traversed = 0u;
      while (linkNode != nullptr && traversed < kMaxLinkNodes &&
             visitedLinkNodes.insert(linkNode).second) {
        ++traversed;
        void* childRuntimeModel = nullptr;
        void* nextLinkNode = nullptr;
        SafeReadPtrFast(linkNode, 8u, childRuntimeModel);
        SafeReadPtrFast(linkNode, 4u, nextLinkNode);
        if (childRuntimeModel != nullptr)
          pending.push_back(childRuntimeModel);
        linkNode = nextLinkNode;
      }
    }
  }

  return out;
}

void RecordRuntimeModelBinding(void *sourcePtr, void *spritePtr) {
  if (!sourcePtr || !spritePtr)
    return;

  void *runtimeModelPtr =
      TryReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model);
  void *modelResourcePtr = NormalizeDirectModelResourcePtr(
      TryReadPtrFast(sourcePtr, kSourceModelResourceOffset));
  const uint32_t sourceFlags = TryReadU32Fast(sourcePtr, kSourceFlagsOffset);

  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  PublishRuntimeSourceObject(runtimeModelPtr, spritePtr, sourcePtr,
                             TryReadSourceSpriteObjectPtr(sourcePtr));

  if (void* ownedModelResourcePtr = NormalizeDirectModelResourcePtr(
          TryResolveDirectModelResourceFromRuntimeModel(runtimeModelPtr))) {
    modelResourcePtr = ownedModelResourcePtr;
  }

  auto &modelRegistry = ModelRegistry::instance();
  auto &instanceRegistry = ModelInstanceRegistry::instance();
  modelRegistry.recordRuntimeModelBinding(spritePtr, runtimeModelPtr,
                                          modelResourcePtr, 0u, sourceFlags);
  instanceRegistry.bindRuntimeModelToSprite(spritePtr, runtimeModelPtr, 0u,
                                            modelResourcePtr);
  ShadowModelResourceCache::instance().noteRuntimeModelBinding(runtimeModelPtr,
                                                               modelResourcePtr,
                                                               0u);
  ShadowModelResourceCache::instance().noteModelResourceBinding(modelResourcePtr,
                                                                0u);

  ModelResourceRecord resourceRecord = {};
  if (modelRegistry.findByRuntimeModel(runtimeModelPtr, resourceRecord)) {
    resourceRecord.modelResourcePtr =
        NormalizeDirectModelResourcePtr(resourceRecord.modelResourcePtr);
    instanceRegistry.bindRuntimeModelToSprite(
        spritePtr, runtimeModelPtr, resourceRecord.modelKey,
        resourceRecord.modelResourcePtr);
    ShadowModelResourceCache::instance().noteRuntimeModelBinding(
        runtimeModelPtr, resourceRecord.modelResourcePtr,
        resourceRecord.modelKey);
    ShadowModelResourceCache::instance().noteModelResourceBinding(
        resourceRecord.modelResourcePtr, resourceRecord.modelKey);
    render::NoteShadowRuntimeModelBinding(
        spritePtr, runtimeModelPtr, resourceRecord.modelResourcePtr,
        resourceRecord.modelPath, resourceRecord.modelType,
        resourceRecord.flags, resourceRecord.modelKey);
    MaybeLogBinding(spritePtr, runtimeModelPtr, resourceRecord.modelResourcePtr,
                    resourceRecord.modelKey);
    return;
  }

  render::NoteShadowRuntimeModelBinding(spritePtr, runtimeModelPtr,
                                        modelResourcePtr, std::string(), 0u,
                                        sourceFlags, 0u);
  MaybeLogBinding(spritePtr, runtimeModelPtr, modelResourcePtr, 0u);
}

void RecordGeosetResource(void *geosetPtr) {
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticGeosetResourceCapture) {
    return;
  }
  if (!geosetPtr)
    return;
  ShadowModelResourceCache::instance().recordGeosetCreate(geosetPtr);
}

uint64_t HashPointerTriple(void* a, void* b, void* c) {
  uint64_t hash = 1469598103934665603ull;
  auto mix = [&](void* ptr) {
    const uint64_t value = uint64_t(reinterpret_cast<uintptr_t>(ptr));
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(a);
  mix(b);
  mix(c);
  return hash;
}

uint64_t HashPointerPair(void* a, void* b) {
  uint64_t hash = 1469598103934665603ull;
  hash ^= uint64_t(reinterpret_cast<uintptr_t>(a));
  hash *= 1099511628211ull;
  hash ^= uint64_t(reinterpret_cast<uintptr_t>(b));
  hash *= 1099511628211ull;
  return hash;
}

bool MarkSpriteFrameSourceIdentityProcessedThisFrame(void* spritePtr,
                                                    void* sourceObjectPtr,
                                                    void* runtimeModelPtr) {
  uint64_t frame = PoseRegistry::instance().frameNumber();
  if (frame == 0u)
    frame = ModelInstanceRegistry::instance().frameNumber();

  struct DedupeState {
    uint64_t frame = 0u;
    std::unordered_set<uint64_t> keys;
  };
  static thread_local DedupeState s_state;
  if (s_state.frame != frame) {
    s_state.frame = frame;
    s_state.keys.clear();
    s_state.keys.reserve(1024u);
  }

  return !s_state.keys
              .insert(HashPointerTriple(spritePtr, sourceObjectPtr,
                                        runtimeModelPtr))
              .second;
}

// Pose Hook 回调帧级去重：同一 (spritePtr, runtimeModelPtr) 组合
// 在同一帧内只需要执行一次完整的注册表查找/合并/写入。
// 后续调用直接跳过，避免每帧数千次冗余 unordered_map 操作。
bool MarkSpriteFramePoseProcessedThisFrame(void* spritePtr,
                                          void* runtimeModelPtr) {
  uint64_t frame = PoseRegistry::instance().frameNumber();
  if (frame == 0u)
    frame = ModelInstanceRegistry::instance().frameNumber();

  struct DedupeState {
    uint64_t frame = 0u;
    std::unordered_set<uint64_t> keys;
  };
  static thread_local DedupeState s_state;
  if (s_state.frame != frame) {
    s_state.frame = frame;
    s_state.keys.clear();
    s_state.keys.reserve(512u);
  }
  return !s_state.keys.insert(HashPointerPair(spritePtr, runtimeModelPtr)).second;
}

// RuntimePose 帧级去重：同一 runtimeModel 同一帧只处理一次根姿态记录。
bool MarkRuntimePoseProcessedThisFrame(void* runtimeModelPtr) {
  uint64_t frame = PoseRegistry::instance().frameNumber();
  if (frame == 0u)
    frame = ModelInstanceRegistry::instance().frameNumber();

  struct DedupeState {
    uint64_t frame = 0u;
    std::unordered_set<uint64_t> keys;
  };
  static thread_local DedupeState s_state;
  if (s_state.frame != frame) {
    s_state.frame = frame;
    s_state.keys.clear();
    s_state.keys.reserve(512u);
  }
  return !s_state.keys
              .insert(uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)))
              .second;
}

void MaybeRecordSpriteFrameSourceIdentity(int spritePtr, int sourceObjectPtr) {
  if (spritePtr == 0 || sourceObjectPtr == 0)
    return;

  void* spriteRaw = reinterpret_cast<void*>(spritePtr);
  void* runtimeModelPtr =
      TryReadTrustedPtr(spriteRaw, dxvk::war3::CSpriteOffsets::Model);
  if (!LooksLikeRuntimeModelPtrCached(runtimeModelPtr))
    return;

  void* sourceObjectRaw =
      reinterpret_cast<void*>(uintptr_t(uint32_t(sourceObjectPtr)));
  if (reinterpret_cast<uintptr_t>(sourceObjectRaw) < 0x10000u) {
    return;
  }

  g_spriteFrameSourceHintCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceObjectRaw)),
      std::memory_order_relaxed);
  g_lastSpriteFrameSourceRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  if (MarkSpriteFrameSourceIdentityProcessedThisFrame(
          spriteRaw, sourceObjectRaw, runtimeModelPtr)) {
    return;
  }

  if (g_config.logEnabled)
    RecordSpriteFrameSourceObjectFieldProbe(sourceObjectRaw, runtimeModelPtr);
  void* sourceSpriteObjectPtr = TryReadSourceSpriteObjectPtr(sourceObjectRaw);
  PublishRuntimeSourceObjectLink(runtimeModelPtr, spriteRaw, sourceObjectRaw,
                                 sourceSpriteObjectPtr);
  void* baseRuntimeAliasPtr =
      g_config.logEnabled ? TryResolveRuntimeModelBaseAlias(runtimeModelPtr)
                          : nullptr;
  if (baseRuntimeAliasPtr != nullptr) {
    PublishRuntimeSourceObjectLink(baseRuntimeAliasPtr, spriteRaw,
                                   sourceObjectRaw, sourceSpriteObjectPtr);
    g_spriteFrameSourceBaseAliasPublishCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastSpriteFrameSourceBaseRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(baseRuntimeAliasPtr)),
        std::memory_order_relaxed);
  }

  ModelInstanceRecord ownerRecord = {};
  ownerRecord.spritePtr = spriteRaw;
  ownerRecord.runtimeModelPtr = runtimeModelPtr;

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  ModelInstanceRecord cachedOwnerRecord = {};
  if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, cachedOwnerRecord) ||
      instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr,
                                               cachedOwnerRecord)) {
    MergeAttachmentIdentityFromInstance(ownerRecord, cachedOwnerRecord);
  }

  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  render::ObjectKind kind = render::ObjectKind::Unknown;
  if (!HasLogicalObjectIdentity(ownerRecord)) {
    TryResolveCurrentRenderOwnerHint(spriteRaw, runtimeModelPtr, ownerRecord,
                                     kind);
  }

  ModelInstanceRecord sourceHintRecord = {};
  uint32_t sourceHintOffset = 0u;
  void* sourceHintCandidatePtr = nullptr;
  bool sourceHintResolved = false;
  if (!HasLogicalObjectIdentity(ownerRecord) && g_config.logEnabled) {
    sourceHintResolved =
        TryResolveSourceObjectIdentityHint(sourceObjectRaw, runtimeModelPtr,
                                           sourceHintRecord, sourceHintOffset,
                                           sourceHintCandidatePtr);
    ModelInstanceRecord deepSourceHintRecord = {};
    uint32_t deepSourceHintOffset = 0u;
    void* deepSourceHintCandidatePtr = nullptr;
    if (!sourceHintResolved &&
        TryResolveSourceObjectIdentityHintDeep(
            sourceObjectRaw, runtimeModelPtr, deepSourceHintRecord,
            deepSourceHintOffset, deepSourceHintCandidatePtr)) {
      sourceHintRecord = deepSourceHintRecord;
      sourceHintOffset = deepSourceHintOffset;
      sourceHintCandidatePtr = deepSourceHintCandidatePtr;
      sourceHintResolved = true;
    }
    if (sourceHintResolved && sourceHintOffset > 0x40u) {
      g_spriteFrameSourceDeepIdentityResolvedCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSpriteFrameSourceDeepIdentityCandidatePtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(sourceHintCandidatePtr)),
          std::memory_order_relaxed);
      g_lastSpriteFrameSourceDeepIdentityOffset.store(
          sourceHintOffset, std::memory_order_relaxed);
    }
    if (sourceHintResolved && HasLogicalObjectIdentity(sourceHintRecord)) {
      MergeAttachmentIdentityFromInstance(ownerRecord, sourceHintRecord);
    }
  }
  if (!HasLogicalObjectIdentity(ownerRecord) && g_config.logEnabled &&
      TryResolveSourceObjectIdentity(sourceObjectRaw, spriteRaw, unitPtr,
                                     jHandle, rawcode, kind)) {
    ownerRecord.unitPtr = unitPtr;
    ownerRecord.jHandle = jHandle;
    ownerRecord.rawcode = rawcode;
  }
  if (!HasLogicalObjectIdentity(ownerRecord) && g_config.logEnabled) {
    render::RenderObjectIdentitySnapshot sourceRenderIdentity = {};
    if (TryResolveSourceObjectRenderIdentity(sourceObjectRaw,
                                             sourceRenderIdentity) &&
        (sourceRenderIdentity.unitPtr != nullptr ||
         sourceRenderIdentity.jHandle != 0u ||
         sourceRenderIdentity.rawcode != 0u)) {
      MergeAttachmentIdentityFromRender(ownerRecord, sourceRenderIdentity);
    }
  }
  if (!HasLogicalObjectIdentity(ownerRecord)) {
    return;
  }

  g_spriteFrameSourceResolvedIdentityCount.fetch_add(1u,
                                                     std::memory_order_relaxed);
  if (ownerRecord.unitPtr != nullptr)
    g_spriteFrameSourceResolvedUnitCount.fetch_add(1u,
                                                   std::memory_order_relaxed);
  if (ownerRecord.jHandle != 0u)
    g_spriteFrameSourceResolvedHandleCount.fetch_add(1u,
                                                     std::memory_order_relaxed);
  if (ownerRecord.rawcode != 0u)
    g_spriteFrameSourceResolvedRawcodeCount.fetch_add(1u,
                                                      std::memory_order_relaxed);
  g_lastSpriteFrameSourceWorldObjectEntryPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.worldObjectEntry)),
      std::memory_order_relaxed);
  g_lastSpriteFrameSourceSceneNodePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.sceneNode)),
      std::memory_order_relaxed);
  g_lastSpriteFrameSourceUnitPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(ownerRecord.unitPtr)),
      std::memory_order_relaxed);
  g_lastSpriteFrameSourceJHandle.store(ownerRecord.jHandle,
                                       std::memory_order_relaxed);
  g_lastSpriteFrameSourceRawcode.store(ownerRecord.rawcode,
                                       std::memory_order_relaxed);

  render::NoteShadowRuntimeIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      spriteRaw, ownerRecord.jHandle, ownerRecord.rawcode, kind);
  instanceRegistry.noteInstanceIdentity(
      ownerRecord.worldObjectEntry, ownerRecord.sceneNode, ownerRecord.unitPtr,
      spriteRaw, ownerRecord.jHandle, ownerRecord.rawcode);
  instanceRegistry.bindRuntimeModelToSprite(spriteRaw, runtimeModelPtr, 0u,
                                            nullptr);
  instanceRegistry.noteRuntimeOwnerIdentity(
      runtimeModelPtr, ownerRecord.worldObjectEntry, ownerRecord.sceneNode,
      ownerRecord.unitPtr, spriteRaw, ownerRecord.jHandle,
      ownerRecord.rawcode);
  if (baseRuntimeAliasPtr != nullptr) {
    instanceRegistry.bindRuntimeModelToSprite(spriteRaw, baseRuntimeAliasPtr, 0u,
                                              nullptr);
    instanceRegistry.noteRuntimeOwnerIdentity(
        baseRuntimeAliasPtr, ownerRecord.worldObjectEntry,
        ownerRecord.sceneNode, ownerRecord.unitPtr, spriteRaw,
        ownerRecord.jHandle, ownerRecord.rawcode);
  }

  if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr, ownerRecord)) {
    ownerRecord.runtimeModelPtr = runtimeModelPtr;
    ownerRecord.spritePtr = spriteRaw;
  }
  RecordRuntimePaletteTreeIfStale(
      int(reinterpret_cast<uintptr_t>(runtimeModelPtr)), &ownerRecord);
  if (baseRuntimeAliasPtr != nullptr) {
    ModelInstanceRecord baseOwnerRecord = ownerRecord;
    baseOwnerRecord.runtimeModelPtr = baseRuntimeAliasPtr;
    baseOwnerRecord.spritePtr = spriteRaw;
    RecordRuntimePaletteTreeIfStale(
        int(reinterpret_cast<uintptr_t>(baseRuntimeAliasPtr)),
        &baseOwnerRecord);
  }
}

constexpr uint32_t kSpriteFrameUpdateKindFull = 0x1u;
constexpr uint32_t kSpriteFrameUpdateKindMini = 0x2u;
constexpr uint32_t kSpriteFrameUpdateKindLite = 0x4u;
constexpr uint32_t kSpriteFrameUpdateKindMiniLite = 0x8u;

// Phase 7.47 dt gate probe: 记录 CSpriteUber_PreRender*(dt) 的输入分布。
// 当 |dt| < FLT_EPSILON (~1.19e-7f) 时，引擎会 skip CModel_EvalPoseStackAndChildren，
// 整条 palette writer 链路不触发。这个 counter 让 full trace 在冻结窗口里
// 能直接看到"producer 早退占比"。
static inline void NoteSpriteUberPreRenderDtBucket(float dt) {
  g_spriteUberPreRenderTotalCount.fetch_add(1u, std::memory_order_relaxed);
  uint32_t bits = 0u;
  std::memcpy(&bits, &dt, sizeof(bits));
  g_spriteUberPreRenderLastDtBits.store(bits, std::memory_order_relaxed);

  uint32_t frameTag = 0u;
  TryReadCurrentPaletteFrameTag(frameTag);

  constexpr float kFltEpsilon = 1.1920929e-7f;  // IDA 里看到的 0.00000023841858 是 2*eps
  const float absDt = dt < 0.0f ? -dt : dt;
  if (bits == 0u) {
    g_spriteUberPreRenderDtZeroCount.fetch_add(1u, std::memory_order_relaxed);
    if (frameTag != 0u)
      g_spriteUberPreRenderLastZeroDtFrameTag.store(
          frameTag, std::memory_order_relaxed);
  } else if (absDt < kFltEpsilon * 2.0f) {
    // 与 IDA 中 0.00000023841858f (2*FLT_EPSILON) 的门槛对齐：
    // 小于该值会被 skip。
    g_spriteUberPreRenderDtBelowEpsilonCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (dt > 0.0f) {
    g_spriteUberPreRenderDtPositiveCount.fetch_add(
        1u, std::memory_order_relaxed);
    if (frameTag != 0u)
      g_spriteUberPreRenderLastPositiveDtFrameTag.store(
          frameTag, std::memory_order_relaxed);
  } else {
    g_spriteUberPreRenderDtNegativeCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
}

// Phase 7.47 writer-per-frame 去重：每个 palette frameTag 第一次触发
// writer hook 时累加 WithHit；frameTag 从上次推进但当前 writer 没被
// 调用过，则下次第一次触发会先记一个 Empty。和 full trace 的 frameTag
// cadence 对齐。
static inline void NoteWriterHitForFrameTag(
    std::atomic<uint32_t>& lastFrameTag,
    std::atomic<uint64_t>& withHitCount,
    std::atomic<uint64_t>& emptyCount,
    uint32_t currentFrameTag) {
  if (currentFrameTag == 0u)
    return;
  uint32_t previous = lastFrameTag.load(std::memory_order_relaxed);
  if (previous == currentFrameTag)
    return;
  // CAS 保证每 frameTag 只会被记一次 WithHit，并发安全。
  if (lastFrameTag.compare_exchange_strong(previous, currentFrameTag,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    withHitCount.fetch_add(1u, std::memory_order_relaxed);
    if (previous != 0u && currentFrameTag > previous + 1u) {
      // 中间跳过 N-1 帧没看到这个 writer：记成 empty。
      emptyCount.fetch_add(uint64_t(currentFrameTag - previous - 1u),
                           std::memory_order_relaxed);
    }
  }
}


void NoteSpriteFrameAttachmentRuntimeHit(void* spritePtr, void* runtimeModelPtr,
                                         void* contextPtr,
                                         uint32_t updateKind,
                                         uintptr_t callerPc) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return;
  }
  if (!g_config.poseEnabled || !g_config.attachmentEnabled)
    return;
  if (spritePtr == nullptr || !LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  auto& attachmentRegistry = AttachmentRigidRegistry::instance();
  AttachmentRigidRecord attachmentRecord = {};
  uint32_t roleMask = 0u;
  if (attachmentRegistry.findByRootRuntimeModel(runtimeModelPtr, attachmentRecord)) {
    roleMask |= 0x1u;
    g_spriteFrameAttachmentRootRuntimeHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (attachmentRegistry.findByOwnerRuntimeModel(runtimeModelPtr, attachmentRecord)) {
    roleMask |= 0x2u;
    g_spriteFrameAttachmentOwnerRuntimeHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (attachmentRegistry.findByChildRuntimeModel(runtimeModelPtr, attachmentRecord)) {
    roleMask |= 0x4u;
    g_spriteFrameAttachmentChildRuntimeHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }

  if (roleMask == 0u)
    return;

  if (contextPtr != nullptr)
    g_spriteFrameAttachmentContextHintCount.fetch_add(
        1u, std::memory_order_relaxed);
  if ((updateKind & (kSpriteFrameUpdateKindFull | kSpriteFrameUpdateKindMini)) !=
      0u) {
    g_spriteFrameAttachmentFullUpdateHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if ((updateKind & (kSpriteFrameUpdateKindLite |
                     kSpriteFrameUpdateKindMiniLite)) != 0u) {
    g_spriteFrameAttachmentLiteUpdateHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  const uint32_t callerRva = GetModuleRvaFromAddress(callerPc);
  if (callerRva != 0u) {
    g_spriteFrameAttachmentCallerKnownCount.fetch_add(
        1u, std::memory_order_relaxed);
    const uint32_t previousCallerRva =
        g_lastSpriteFrameAttachmentCallerRva.exchange(
            callerRva, std::memory_order_relaxed);
    if (previousCallerRva != 0u && previousCallerRva != callerRva) {
      g_spriteFrameAttachmentCallerChangedCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }

  if ((roleMask & 0x2u) != 0u) {
    MaybePublishAttachedEffectInitParentRuntimeOwnerIdentity(spritePtr,
                                                             runtimeModelPtr);
  }

  if (g_attachModelToPointScopeState.depth != 0u) {
    g_spriteFrameAttachmentAttachScopeHitCount.fetch_add(
        1u, std::memory_order_relaxed);
    if ((roleMask & 0x2u) != 0u) {
      g_spriteFrameAttachmentAttachScopeOwnerHitCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (g_attachModelToPointScopeState.parentRuntimeModelPtr == runtimeModelPtr) {
      g_spriteFrameAttachmentAttachScopeParentRuntimeMatchCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
    g_lastAttachScopeCallerRva.store(g_attachModelToPointScopeState.callerRva,
                                     std::memory_order_relaxed);
    g_lastAttachScopeParentSpritePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(
            g_attachModelToPointScopeState.parentSpritePtr)),
        std::memory_order_relaxed);
    g_lastAttachScopeParentRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(
            g_attachModelToPointScopeState.parentRuntimeModelPtr)),
        std::memory_order_relaxed);
    g_lastAttachScopeChildSpritePtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(
            g_attachModelToPointScopeState.childSpritePtr)),
        std::memory_order_relaxed);
    void* scopeChildRuntimeModelPtr =
        TryReadPtrFast(g_attachModelToPointScopeState.childSpritePtr,
                       dxvk::war3::CSpriteOffsets::Model);
    if (!LooksLikeRuntimeModelPtr(scopeChildRuntimeModelPtr))
      scopeChildRuntimeModelPtr = nullptr;
    g_lastAttachScopeChildRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(scopeChildRuntimeModelPtr)),
        std::memory_order_relaxed);
    g_lastAttachScopeHitRuntimeModelPtr.store(
        uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
        std::memory_order_relaxed);
    g_lastAttachScopeHitRoleMask.store(roleMask,
                                       std::memory_order_relaxed);
  }

  g_lastSpriteFrameAttachmentSpritePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(spritePtr)),
      std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentRuntimeModelPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
      std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentContextPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(contextPtr)),
      std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentRoleMask.store(roleMask,
                                            std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentUpdateKind.store(updateKind,
                                              std::memory_order_relaxed);
}

void RecordSpriteFrameRuntimeModelBindingLite(void* spritePtr,
                                              void* runtimeModelPtr) {
  if (spritePtr == nullptr || !LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  void* modelResourcePtr = NormalizeDirectModelResourcePtr(
      TryResolveDirectModelResourceFromRuntimeModel(runtimeModelPtr));

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  instanceRegistry.bindRuntimeModelToSprite(spritePtr, runtimeModelPtr, 0u,
                                            modelResourcePtr,
                                            false /* propagateOwnerIdentity */);

  auto& modelRegistry = ModelRegistry::instance();
  if (modelResourcePtr != nullptr) {
    modelRegistry.recordRuntimeModelBinding(spritePtr, runtimeModelPtr,
                                            modelResourcePtr, 0u, 0u);
  }

  auto& resourceCache = ShadowModelResourceCache::instance();
  ShadowModelResourceRecord existingRuntimeResource = {};
  const bool hasExistingRuntimeResource =
      resourceCache.findRuntimeModelResource(runtimeModelPtr,
                                             existingRuntimeResource);
  const bool needsRuntimeResourcePublish =
      !hasExistingRuntimeResource ||
      (existingRuntimeResource.modelResourcePtr == nullptr &&
       modelResourcePtr != nullptr) ||
      existingRuntimeResource.readyGeosetCount == 0u;
  if (needsRuntimeResourcePublish) {
    resourceCache.noteRuntimeModelBinding(runtimeModelPtr, modelResourcePtr, 0u);
    if (modelResourcePtr != nullptr)
      resourceCache.noteModelResourceBinding(modelResourcePtr, 0u);
  }

  // The lite path is intentionally hot-path minimal. It already publishes the
  // runtime/resource facts needed by the direct CModel pose contract above;
  // calling the full shadow bridge here would duplicate registry writes and
  // can re-enter owner propagation for every sprite-frame update.
}

void RecordRuntimePose(int runtimeModel, const __m128i *poseMatrix, float scale) {
  if (runtimeModel == 0)
    return;

  // 帧级去重：同一 runtimeModel 同一帧只处理一次根姿态记录
  if (MarkRuntimePoseProcessedThisFrame(reinterpret_cast<void*>(runtimeModel)))
    return;

  // 直读快路径：跳过 Registry 查找，CModel pose 已在 SpriteFrameUpdate 快路径中采集
  if constexpr (dxvk::war3::internal::kWar3SemanticDirectCModelPoseEnabled) {
    return;
  }

  const Matrix4 worldTransform = DecodeRuntimePoseMatrix(poseMatrix);
  ModelInstanceRecord instanceRecord = {};
  if (!ModelInstanceRegistry::instance().findByRuntimeModel(
          reinterpret_cast<void *>(runtimeModel), instanceRecord)) {
    render::NoteShadowRuntimePose(reinterpret_cast<void *>(runtimeModel),
                                  nullptr, nullptr, 0u, 0.0f, scale, 0.0f,
                                  0.0f, 0.0f, 0.0f, true, &worldTransform);
    MaybeLogPose(reinterpret_cast<void *>(runtimeModel), nullptr, nullptr,
                 scale);
    return;
  }

  float flyHeight = 0.0f;
  if (instanceRecord.unitPtr) {
    game::UnitWrapper unit(instanceRecord.unitPtr);
    if (unit.IsValid())
      flyHeight = unit.GetFlyHeight();
  }

  render::NoteShadowRuntimePose(reinterpret_cast<void *>(runtimeModel),
                                instanceRecord.sceneNode,
                                instanceRecord.unitPtr, 0u, 0.0f, scale,
                                0.0f, 0.0f, 0.0f, flyHeight, true,
                                &worldTransform);
  MaybeLogPose(reinterpret_cast<void *>(runtimeModel), instanceRecord.sceneNode,
               instanceRecord.unitPtr, scale);
}

uint64_t PublishRuntimeMatrixPalette(int runtimeModel,
                                     const std::vector<Matrix4>& matrices,
                                     bool allowResourceBinding) {
  if (runtimeModel == 0)
    return 0u;

  RememberRuntimePoseArrayRange(runtimeModel);

  // 直读快路径：跳过 PoseRegistry/ModelRegistry 查找和矩阵拷贝
  if constexpr (dxvk::war3::internal::kWar3SemanticDirectCModelPoseEnabled) {
    return 0u;
  }

  if (matrices.empty())
    return 0u;

  const void* runtimeModelPtr = reinterpret_cast<void*>(runtimeModel);

  const uint64_t matrixHash =
      matrices.empty() ? 0ull : HashMatrixPalette(matrices);

  ModelInstanceRecord instanceRecord = {};
  ModelInstanceRegistry::instance().findByRuntimeModel(
      const_cast<void*>(runtimeModelPtr), instanceRecord);

  void* ownedModelResourcePtr =
      allowResourceBinding
          ? NormalizeDirectModelResourcePtr(
                TryResolveDirectModelResourceFromRuntimeModel(
                    const_cast<void*>(runtimeModelPtr)))
          : nullptr;
  if (ownedModelResourcePtr != nullptr) {
    auto& modelRegistry = ModelRegistry::instance();
    ModelResourceRecord resourceRecord = {};
    modelRegistry.findByRuntimeModel(const_cast<void*>(runtimeModelPtr),
                                     resourceRecord);
    resourceRecord.modelResourcePtr =
        NormalizeDirectModelResourcePtr(resourceRecord.modelResourcePtr);
    if (instanceRecord.spritePtr != nullptr) {
      modelRegistry.recordRuntimeModelBinding(
          instanceRecord.spritePtr, const_cast<void*>(runtimeModelPtr),
          ownedModelResourcePtr, resourceRecord.modelType, resourceRecord.flags);
    }
    if (modelRegistry.findByRuntimeModel(const_cast<void*>(runtimeModelPtr),
                                         resourceRecord)) {
      resourceRecord.modelResourcePtr =
          NormalizeDirectModelResourcePtr(resourceRecord.modelResourcePtr);
      if (instanceRecord.spritePtr != nullptr) {
        ModelInstanceRegistry::instance().bindRuntimeModelToSprite(
            instanceRecord.spritePtr, const_cast<void*>(runtimeModelPtr),
            resourceRecord.modelKey, resourceRecord.modelResourcePtr);
      }
      ShadowModelResourceCache::instance().noteRuntimeModelBinding(
          const_cast<void*>(runtimeModelPtr), resourceRecord.modelResourcePtr,
          resourceRecord.modelKey);
      ShadowModelResourceCache::instance().noteModelResourceBinding(
          resourceRecord.modelResourcePtr, resourceRecord.modelKey);
      if (instanceRecord.spritePtr != nullptr) {
        render::NoteShadowRuntimeModelBinding(
            instanceRecord.spritePtr, const_cast<void*>(runtimeModelPtr),
            resourceRecord.modelResourcePtr, resourceRecord.modelPath,
            resourceRecord.modelType, resourceRecord.flags,
            resourceRecord.modelKey);
      }
    } else {
      ShadowModelResourceCache::instance().noteRuntimeModelBinding(
          const_cast<void*>(runtimeModelPtr), ownedModelResourcePtr, 0u);
      ShadowModelResourceCache::instance().noteModelResourceBinding(
          ownedModelResourcePtr, 0u);
    }
  }

  PoseRegistry::instance().recordMatrixPalette(
      const_cast<void*>(runtimeModelPtr), instanceRecord.sceneNode,
      instanceRecord.unitPtr, matrices.data(), uint32_t(matrices.size()));
  Matrix4 worldTransform;
  if (TryReadRuntimeWorldTransform(runtimeModel, worldTransform)) {
    PoseRegistry::instance().recordPose(
        const_cast<void*>(runtimeModelPtr), instanceRecord.sceneNode,
        instanceRecord.unitPtr, 0u, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        true, &worldTransform);
  }
  render::NoteShadowRuntimePose(
      const_cast<void*>(runtimeModelPtr), instanceRecord.sceneNode,
      instanceRecord.unitPtr, 0u, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false,
      nullptr, uint32_t(matrices.size()), matrixHash);
  g_runtimeMatrixPublisherPoseRevision.fetch_add(1u,
                                                 std::memory_order_relaxed);
  return matrixHash;
}

bool RecordRuntimeMatrixPalette(int runtimeModel,
                                bool allowResourceBinding,
                                uint32_t* outMatrixCount,
                                uint64_t* outMatrixHash) {
  std::vector<Matrix4> matrices;
  if (!TryReadRuntimeMatrixPalette(runtimeModel, matrices))
    return false;

  const uint64_t matrixHash =
      PublishRuntimeMatrixPalette(runtimeModel, matrices, allowResourceBinding);
  if (outMatrixCount != nullptr)
    *outMatrixCount = uint32_t(matrices.size());
  if (outMatrixHash != nullptr)
    *outMatrixHash = matrixHash;
  return true;
}

bool RecordRuntimeMatrixPaletteFromRangeCopy(int runtimeModel, int contextPtr,
                                             int sourceMatrixBasePtr,
                                             bool allowResourceBinding,
                                             bool publishPalette) {
  std::vector<Matrix4> matrices;
  if (!TryReadRuntimeMatrixPaletteFromRangeCopy(runtimeModel, contextPtr,
                                                sourceMatrixBasePtr,
                                                matrices)) {
    g_runtimeMatrixRangeCopyPalettePublishMissCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  const uint64_t matrixHash = HashMatrixPalette(matrices);
  g_runtimeMatrixRangeCopyPalettePublishHitCount.fetch_add(
      1u, std::memory_order_relaxed);
  g_runtimeMatrixRangeCopyLastRuntimeModelPtr.store(
      uint64_t(uint32_t(runtimeModel)), std::memory_order_relaxed);
  g_runtimeMatrixRangeCopyLastContextPtr.store(
      uint64_t(uint32_t(contextPtr)), std::memory_order_relaxed);
  g_runtimeMatrixRangeCopyLastSourceBasePtr.store(
      uint64_t(uint32_t(sourceMatrixBasePtr)), std::memory_order_relaxed);
  g_runtimeMatrixRangeCopyLastMatrixCount.store(
      uint64_t(matrices.size()), std::memory_order_relaxed);
  g_runtimeMatrixRangeCopyLastMatrixHash.store(matrixHash,
                                               std::memory_order_relaxed);
  if (publishPalette) {
    // Phase 7.34 A3 性能优化：per-runtimeModel 的 lastHash 快退。
    // 同一模型连续帧如果 palette 完全没变（静态模型 / 同一动画帧重复调用），
    // PublishRuntimeMatrixPalette 的 ModelRegistry 查询 + PoseRegistry 录入
    // 重复做了就是白费。用一张固定大小的 TLS-safe 表做 hash 比对快退。
    // 命中快退时只更新轻量 counter，跳过所有锁/查询/拷贝。
    //
    // 回退开关：`DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH_DEDUP=0` 可禁用。
    static const bool s_publishDedupEnabled =
        GetEnvBoolCached("DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH_DEDUP",
                         true);
    if (s_publishDedupEnabled) {
      struct PublishDedupEntry {
        int runtimeModel = 0;
        uint64_t hash = 0u;
      };
      static constexpr size_t kDedupSize = 512u;
      thread_local std::array<PublishDedupEntry, kDedupSize> s_dedupTable{};
      const size_t slot =
          (static_cast<size_t>(static_cast<uint32_t>(runtimeModel)) >> 4u) %
          kDedupSize;
      auto& entry = s_dedupTable[slot];
      if (entry.runtimeModel == runtimeModel && entry.hash == matrixHash) {
        // Hash 一致：跳过 PublishRuntimeMatrixPalette，数据本就没变。
        g_runtimeMatrixRangeCopyPublishSkippedDedupCount.fetch_add(
            1u, std::memory_order_relaxed);
        return true;
      }
      entry.runtimeModel = runtimeModel;
      entry.hash = matrixHash;
    }
    PublishRuntimeMatrixPalette(runtimeModel, matrices, allowResourceBinding);
  }
  return true;
}

bool MarkRuntimePaletteTreeProcessedThisFrame(void* runtimeModelPtr,
                                             bool ownerHintHasIdentity) {
  if (runtimeModelPtr == nullptr)
    return false;

  uint64_t frame = PoseRegistry::instance().frameNumber();
  if (frame == 0u)
    frame = ModelInstanceRegistry::instance().frameNumber();

  std::lock_guard<std::mutex> lock(g_runtimePaletteTreeDedupeMutex);
  if (g_runtimePaletteTreeDedupeFrame != frame) {
    g_runtimePaletteTreeDedupeFrame = frame;
    g_runtimePaletteTreeDedupeRoots.clear();
    g_runtimePaletteTreeDedupeOwnerRoots.clear();
    g_runtimePaletteTreeDedupeRoots.reserve(512u);
    g_runtimePaletteTreeDedupeOwnerRoots.reserve(512u);
  }
  auto& roots = ownerHintHasIdentity ? g_runtimePaletteTreeDedupeOwnerRoots
                                     : g_runtimePaletteTreeDedupeRoots;
  return !roots.insert(runtimeModelPtr).second;
}

void RecordRuntimePaletteTree(int runtimeModel,
                              const ModelInstanceRecord* ownerHint = nullptr) {
  if (runtimeModel == 0)
    return;

  void* rootRuntimeModelPtr = reinterpret_cast<void*>(runtimeModel);
  const bool ownerHintHasIdentity =
      ownerHint != nullptr && HasAttachmentIdentity(*ownerHint);
  if (MarkRuntimePaletteTreeProcessedThisFrame(rootRuntimeModelPtr,
                                              ownerHintHasIdentity)) {
    return;
  }

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  auto& resourceCache = ShadowModelResourceCache::instance();
  ModelInstanceRecord ownerRecord = {};
  bool hasOwnerIdentity = false;
  if (ownerHint != nullptr) {
    ownerRecord = *ownerHint;
    hasOwnerIdentity = HasAttachmentIdentity(ownerRecord);
  }
  if (!hasOwnerIdentity &&
      instanceRegistry.findByRuntimeModel(reinterpret_cast<void*>(runtimeModel),
                                          ownerRecord)) {
    hasOwnerIdentity = HasAttachmentIdentity(ownerRecord);
  }

  // This hook can fire many times while Blizzard walks the same runtime tree.
  // Keep the traversal single-pass: owner propagation, resource binding, and
  // palette publication all consume the same collected child list.
  const auto runtimeModels = CollectRuntimeModelTree(rootRuntimeModelPtr);
  for (void* runtimeModelPtr : runtimeModels) {
    if (runtimeModelPtr == nullptr)
      continue;

    if (hasOwnerIdentity) {
      instanceRegistry.noteRuntimeOwnerIdentity(
          runtimeModelPtr, ownerRecord.worldObjectEntry, ownerRecord.sceneNode,
          ownerRecord.unitPtr, ownerRecord.spritePtr, ownerRecord.jHandle,
          ownerRecord.rawcode);
    }

    void* modelResourcePtr =
        NormalizeDirectModelResourcePtr(
            TryResolveDirectModelResourceFromRuntimeModel(runtimeModelPtr));
    resourceCache.noteRuntimeModelBinding(runtimeModelPtr, modelResourcePtr, 0u);
    if (modelResourcePtr != nullptr)
      resourceCache.noteModelResourceBinding(modelResourcePtr, 0u);

    RecordRuntimeMatrixPalette(int(reinterpret_cast<uintptr_t>(runtimeModelPtr)),
                               false);
  }
}

bool RuntimePaletteKnownForCurrentFrame(void* runtimeModelPtr) {
  if (runtimeModelPtr == nullptr)
    return false;

  PoseRecord poseRecord = {};
  if (!PoseRegistry::instance().findByRuntimeModel(runtimeModelPtr, poseRecord))
    return false;

  return poseRecord.lastMatrixPaletteFrame == PoseRegistry::instance().frameNumber() &&
         poseRecord.matrixCount != 0u &&
         !poseRecord.matrixPalette.empty();
}

void RecordRuntimePaletteTreeIfStale(int runtimeModel,
                                     const ModelInstanceRecord* ownerHint) {
  if (runtimeModel == 0)
    return;

  void* runtimeModelPtr = reinterpret_cast<void*>(runtimeModel);
  if (RuntimePaletteKnownForCurrentFrame(runtimeModelPtr))
    return;

  RecordRuntimePaletteTree(runtimeModel, ownerHint);
}

void RecordSpriteFramePoseFromSprite(int spritePtr, float dt, void* contextPtr,
                                     uint32_t updateKind,
                                     uintptr_t callerPc) {
  if (spritePtr == 0)
    return;

  void* spriteRaw = reinterpret_cast<void*>(spritePtr);
  void* runtimeModelPtr =
      TryReadPtrFast(spriteRaw, dxvk::war3::CSpriteOffsets::Model);
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  // 帧级去重：同一 (spritePtr, runtimeModelPtr) 组合在同一帧内只处理一次。
  // 这是消除 semantic data layer 卡顿的核心修复：4 个 Sprite Frame Update
  // Hook 每帧对每个 Sprite 都触发，但完整的注册表查找/合并/写入操作
  // 只需要执行一次。后续重复调用直接跳过。
  if (MarkSpriteFramePoseProcessedThisFrame(spriteRaw, runtimeModelPtr))
    return;

  if (!g_config.poseEnabled) {
    // With pose hooks disabled, keep SpriteFrameUpdate to the cheapest useful
    // publication only: runtimeModel -> modelResource/runtime geosets. Avoid
    // the multi-index instance/shadow registries here; they were the source of
    // the per-sprite map/mutex storm.
    void* modelResourcePtr = NormalizeDirectModelResourcePtr(
        TryResolveDirectModelResourceFromRuntimeModel(runtimeModelPtr));
    auto& resourceCache = ShadowModelResourceCache::instance();
    ShadowModelResourceRecord existingRuntimeResource = {};
    const bool hasExistingRuntimeResource =
        resourceCache.findRuntimeModelResource(runtimeModelPtr,
                                               existingRuntimeResource);
    const bool needsRuntimeResourcePublish =
        !hasExistingRuntimeResource ||
        (existingRuntimeResource.modelResourcePtr == nullptr &&
         modelResourcePtr != nullptr) ||
        existingRuntimeResource.readyGeosetCount == 0u;
    if (needsRuntimeResourcePublish) {
      if (modelResourcePtr != nullptr) {
        ShadowModelResourceRecord existingModelResource = {};
        const bool hasExistingModelResource =
            resourceCache.findModelResource(modelResourcePtr,
                                            existingModelResource);
        if (!hasExistingModelResource ||
            !existingModelResource.readyForShadowConsumer()) {
          resourceCache.noteModelResourceBinding(modelResourcePtr, 0u);
        }
      }
      resourceCache.noteRuntimeModelBinding(runtimeModelPtr, modelResourcePtr,
                                            0u);
    }
    return;
  }

  // ====== 直读 CModel 快路径 ======
  // 当启用时，完全跳过下方 200+ 行的 Registry 交叉查找链
  // （5 个 Registry × 6 个多索引 Map × mutex），改为直接从 CModel
  // 结构体 O(1) 读取 FinalPoseMatrixArray / WorldMatrix3x4。
  if constexpr (dxvk::war3::internal::kWar3SemanticDirectCModelPoseEnabled) {
    DirectPoseCache::instance().noteSpritePose(
        spriteRaw, runtimeModelPtr,
        /*sceneNode=*/nullptr, /*unitPtr=*/nullptr,
        /*worldObjectEntry=*/nullptr,
        /*rawcode=*/0u, /*jHandle=*/0u, /*flyHeight=*/0.0f);
    return;
  }

  void* baseRuntimeAliasPtr = TryResolveRuntimeModelBaseAlias(runtimeModelPtr);

  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseSpriteAttachmentHit);
    NoteSpriteFrameAttachmentRuntimeHit(spriteRaw, runtimeModelPtr, contextPtr,
                                        updateKind, callerPc);
    if (baseRuntimeAliasPtr != nullptr) {
      NoteSpriteFrameAttachmentRuntimeHit(spriteRaw, baseRuntimeAliasPtr,
                                          contextPtr, updateKind, callerPc);
    }
  }

  Matrix4 worldTransform;
  float scale = 1.0f;
  float sequenceTime = 0.0f;
  bool hasWorldTransform = false;
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseSpriteTransformRead);
    hasWorldTransform =
        TryReadSpriteFrameTransform(spriteRaw, worldTransform, scale,
                                    sequenceTime);
  }

  auto& instanceRegistry = ModelInstanceRegistry::instance();
  ModelInstanceRecord instanceRecord = {};
  uint64_t modelKey = 0u;
  void* modelResourcePtr = nullptr;
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseSpriteIdentityLookup);
    if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord))
      instanceRegistry.findBySpritePtr(spriteRaw, instanceRecord);
    if (!HasAttachmentIdentity(instanceRecord)) {
      ModelInstanceRecord parentOwnerRecord = {};
      if (TryResolveParentSpriteOwnerHint(spriteRaw, runtimeModelPtr,
                                          parentOwnerRecord)) {
        instanceRegistry.noteInstanceIdentity(
            parentOwnerRecord.worldObjectEntry, parentOwnerRecord.sceneNode,
            parentOwnerRecord.unitPtr, spriteRaw, parentOwnerRecord.jHandle,
            parentOwnerRecord.rawcode);
        instanceRegistry.bindRuntimeModelToSprite(
            spriteRaw, runtimeModelPtr, parentOwnerRecord.modelKey,
            parentOwnerRecord.modelResourcePtr);
        instanceRegistry.noteRuntimeOwnerIdentity(
            runtimeModelPtr, parentOwnerRecord.worldObjectEntry,
            parentOwnerRecord.sceneNode, parentOwnerRecord.unitPtr, spriteRaw,
            parentOwnerRecord.jHandle, parentOwnerRecord.rawcode);
        if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr,
                                                 instanceRecord)) {
          instanceRecord = parentOwnerRecord;
        }
      }
    }
    if (instanceRecord.spritePtr == nullptr)
      instanceRecord.spritePtr = spriteRaw;
    if (instanceRecord.runtimeModelPtr == nullptr)
      instanceRecord.runtimeModelPtr = runtimeModelPtr;

    modelKey = instanceRecord.modelKey;
    modelResourcePtr =
        NormalizeDirectModelResourcePtr(instanceRecord.modelResourcePtr);
    ModelResourceRecord modelRecord = {};
    if (ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                     modelRecord)) {
      if (modelKey == 0u)
        modelKey = modelRecord.modelKey;
      if (modelResourcePtr == nullptr) {
        modelResourcePtr =
            NormalizeDirectModelResourcePtr(modelRecord.modelResourcePtr);
      }
    }
    instanceRegistry.bindRuntimeModelToSprite(spriteRaw, runtimeModelPtr,
                                              modelKey, modelResourcePtr);
    instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord);
  }

  ModelInstanceRecord baseInstanceRecord = instanceRecord;
  if (baseRuntimeAliasPtr != nullptr) {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::PoseHook,
                               render::SemanticDataPerfTag::PoseSpriteBaseAlias);
    ModelInstanceRecord existingBaseRecord = {};
    if (instanceRegistry.findByRuntimeModel(baseRuntimeAliasPtr,
                                            existingBaseRecord) ||
        instanceRegistry.findOwnerByRuntimeModel(baseRuntimeAliasPtr,
                                                 existingBaseRecord)) {
      MergeAttachmentIdentityFromInstance(baseInstanceRecord,
                                          existingBaseRecord);
    }
    baseInstanceRecord.runtimeModelPtr = baseRuntimeAliasPtr;
    baseInstanceRecord.spritePtr = spriteRaw;

    uint64_t baseModelKey = modelKey;
    void* baseModelResourcePtr = modelResourcePtr;
    ModelResourceRecord baseModelRecord = {};
    if (ModelRegistry::instance().findByRuntimeModel(baseRuntimeAliasPtr,
                                                     baseModelRecord)) {
      if (baseModelKey == 0u)
        baseModelKey = baseModelRecord.modelKey;
      if (baseModelResourcePtr == nullptr) {
        baseModelResourcePtr =
            NormalizeDirectModelResourcePtr(baseModelRecord.modelResourcePtr);
      }
    }
    ShadowModelResourceRecord baseRuntimeResource = {};
    if (ShadowModelResourceCache::instance().findRuntimeModelResource(
            baseRuntimeAliasPtr, baseRuntimeResource)) {
      if (baseModelKey == 0u)
        baseModelKey = baseRuntimeResource.modelKey;
      if (baseModelResourcePtr == nullptr) {
        baseModelResourcePtr =
            NormalizeDirectModelResourcePtr(baseRuntimeResource.modelResourcePtr);
      }
    }
    if (baseModelResourcePtr != nullptr) {
      baseInstanceRecord.modelResourcePtr = baseModelResourcePtr;
      baseInstanceRecord.modelKey = baseModelKey;
      ShadowModelResourceCache::instance().noteRuntimeModelBinding(
          baseRuntimeAliasPtr, baseModelResourcePtr, baseModelKey);
      ShadowModelResourceCache::instance().noteModelResourceBinding(
          baseModelResourcePtr, baseModelKey);
    }
    if (baseInstanceRecord.sourceObjectPtr != nullptr ||
        baseInstanceRecord.sourceSpriteObjectPtr != nullptr) {
      instanceRegistry.noteRuntimeSourceObject(
          baseRuntimeAliasPtr, baseInstanceRecord.sourceObjectPtr,
          baseInstanceRecord.sourceSpriteObjectPtr, spriteRaw);
    }
    instanceRegistry.bindRuntimeModelToSprite(spriteRaw, baseRuntimeAliasPtr,
                                              baseModelKey,
                                              baseModelResourcePtr);
    if (HasAttachmentIdentity(baseInstanceRecord)) {
      instanceRegistry.noteRuntimeOwnerIdentity(
          baseRuntimeAliasPtr, baseInstanceRecord.worldObjectEntry,
          baseInstanceRecord.sceneNode, baseInstanceRecord.unitPtr, spriteRaw,
          baseInstanceRecord.jHandle, baseInstanceRecord.rawcode);
    }
  }

  float flyHeight = 0.0f;
  if (instanceRecord.unitPtr) {
    game::UnitWrapper unit(instanceRecord.unitPtr);
    if (unit.IsValid())
      flyHeight = unit.GetFlyHeight();
  }

  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::PoseHook,
                               render::SemanticDataPerfTag::PoseSpritePublishPose);
    render::NoteShadowRuntimeSpriteFramePose(
        runtimeModelPtr, spriteRaw, instanceRecord.sceneNode,
        instanceRecord.unitPtr, dt, 0u, sequenceTime, scale, 0.0f, 0.0f, 0.0f,
        flyHeight, hasWorldTransform,
        hasWorldTransform ? &worldTransform : nullptr, 0u, 0ull);
  }
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::PoseHook,
                               render::SemanticDataPerfTag::PoseSpritePaletteGate);
    RecordRuntimeMatrixPalette(
        int(reinterpret_cast<uintptr_t>(runtimeModelPtr)), false);
  }
  if (baseRuntimeAliasPtr != nullptr) {
    {
      SemanticHookPerfScope perf(
          render::SemanticDataPerfTag::PoseHook,
          render::SemanticDataPerfTag::PoseSpritePublishPose);
      render::NoteShadowRuntimeSpriteFramePose(
          baseRuntimeAliasPtr, spriteRaw, baseInstanceRecord.sceneNode,
          baseInstanceRecord.unitPtr, dt, 0u, sequenceTime, scale, 0.0f, 0.0f,
          0.0f, flyHeight, hasWorldTransform,
          hasWorldTransform ? &worldTransform : nullptr, 0u, 0ull);
    }
    {
      SemanticHookPerfScope perf(
          render::SemanticDataPerfTag::PoseHook,
          render::SemanticDataPerfTag::PoseSpritePaletteGate);
      RecordRuntimeMatrixPalette(
          int(reinterpret_cast<uintptr_t>(baseRuntimeAliasPtr)), false);
      g_spriteFramePoseBaseAliasPublishCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastSpriteFramePoseBaseRuntimeModelPtr.store(
          uint64_t(reinterpret_cast<uintptr_t>(baseRuntimeAliasPtr)),
          std::memory_order_relaxed);
      PoseRecord basePoseRecord = {};
      if (PoseRegistry::instance().findByRuntimeModel(baseRuntimeAliasPtr,
                                                      basePoseRecord) &&
          basePoseRecord.matrixCount != 0u &&
          !basePoseRecord.matrixPalette.empty()) {
        g_spriteFramePoseBaseAliasMatrixPaletteCount.fetch_add(
            1u, std::memory_order_relaxed);
        g_lastSpriteFramePoseBaseMatrixCount.store(
            basePoseRecord.matrixCount, std::memory_order_relaxed);
      }
    }
  }

  MaybeLogSpriteFrame(spriteRaw, runtimeModelPtr, instanceRecord.sceneNode,
                      instanceRecord.unitPtr, dt, 0u);
}

int __fastcall Hook_CreateSpriteAndBindSourceObject(void* thisPtr, void* edx,
                                                    void* sourceObjectPtr,
                                                    char a3, int a4, int a5,
                                                    int16_t a6) {
  if (!g_trampolineCreateSpriteAndBindSourceObject)
    return 0;

  g_spriteHostBindCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastSpriteHostSourceObjectPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sourceObjectPtr)),
      std::memory_order_relaxed);
  const int result = g_trampolineCreateSpriteAndBindSourceObject(
      thisPtr, sourceObjectPtr, a3, a4, a5, a6);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelSpriteHostBind);
    RecordSpriteHostOwnerBinding(thisPtr, sourceObjectPtr);
  }
  return result;
}

int __fastcall Hook_AttachedEffectInit(void* thisPtr, void* edx,
                                       void* ownerWidgetPtr, int a3, int a4,
                                       int16_t a5, int a6, int a7,
                                       unsigned int a8) {
  if (!g_trampolineAttachedEffectInit)
    return 0;

  const auto previousScope = g_attachedEffectInitScopeState;
  g_attachedEffectInitScopeState.depth = previousScope.depth + 1u;
  g_attachedEffectInitScopeState.effectPtr = thisPtr;
  g_attachedEffectInitScopeState.ownerWidgetPtr = ownerWidgetPtr;
  const int result = g_trampolineAttachedEffectInit(thisPtr, ownerWidgetPtr, a3,
                                                    a4, a5, a6, a7, a8);
  g_attachedEffectInitScopeState = previousScope;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return result;
  }
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::AttachmentHook,
        render::SemanticDataPerfTag::AttachmentAttachedEffectInit);
    RecordAttachedEffectInitOwnerBinding(thisPtr, ownerWidgetPtr);
  }
  return result;
}

unsigned int __fastcall Hook_AttachedEffectDirectAttach(
    void* thisPtr, void* edx, void* ownerWidgetPtr, int16_t attachPointIndex,
    int attachPointArrayPtr, unsigned int attachPointCount) {
  if (!g_trampolineAttachedEffectDirectAttach)
    return 0u;

  const unsigned int result = g_trampolineAttachedEffectDirectAttach(
      thisPtr, ownerWidgetPtr, attachPointIndex, attachPointArrayPtr,
      attachPointCount);
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return result;
  }
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::AttachmentHook,
        render::SemanticDataPerfTag::AttachmentAttachedEffectDirect);
    RecordAttachedEffectDirectOwnerBinding(thisPtr, ownerWidgetPtr);
  }
  return result;
}

void __fastcall Hook_AttachModelToPoint(void* parentSpritePtr,
                                        int attachPointIndex,
                                        void* childSpritePtr) {
  if (g_trampolineAttachModelToPoint == nullptr)
    return;

  const uintptr_t callerPc = GetCallReturnAddress();
  const uint32_t callerRva = GetModuleRvaFromAddress(callerPc);
  const auto previousScope = g_attachModelToPointScopeState;
  g_attachModelToPointScopeState.depth = previousScope.depth + 1u;
  g_attachModelToPointScopeState.callerRva = callerRva;
  g_attachModelToPointScopeState.parentSpritePtr = parentSpritePtr;
  g_attachModelToPointScopeState.parentRuntimeModelPtr =
      TryReadPtrFast(parentSpritePtr, dxvk::war3::CSpriteOffsets::Model);
  if (!LooksLikeRuntimeModelPtr(
          g_attachModelToPointScopeState.parentRuntimeModelPtr)) {
    g_attachModelToPointScopeState.parentRuntimeModelPtr = nullptr;
  }
  g_attachModelToPointScopeState.childSpritePtr = childSpritePtr;
  g_trampolineAttachModelToPoint(parentSpritePtr, attachPointIndex,
                                 childSpritePtr);
  g_attachModelToPointScopeState = previousScope;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return;
  }
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::AttachmentHook,
        render::SemanticDataPerfTag::AttachmentAttachModelToPoint);
    RecordAttachModelToPointOwnerBinding(parentSpritePtr, attachPointIndex,
                                         childSpritePtr);
  }
}

void *__fastcall Hook_CreateSpriteRuntime(void *thisPtr, void *edx) {
  if (!g_trampolineCreateSpriteRuntime)
    return nullptr;

  void *spritePtr = g_trampolineCreateSpriteRuntime(thisPtr);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelRuntimeModelBinding);
    RecordRuntimeModelBinding(thisPtr, spritePtr);
  }
  return spritePtr;
}

void *__fastcall Hook_CreateGeosetFromRawArrays(int a1, int a2, int a3, int a4,
                                                int a5, int a6, int a7, int a8) {
  if (!g_trampolineCreateGeosetFromRawArrays)
    return nullptr;

  void *geosetPtr =
      g_trampolineCreateGeosetFromRawArrays(a1, a2, a3, a4, a5, a6, a7, a8);
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticGeosetResourceCapture) {
    return geosetPtr;
  }
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelGeosetResource);
    RecordGeosetResource(geosetPtr);
  }
  return geosetPtr;
}

void* __fastcall Hook_RuntimeModelPlainCtor(void* thisPtr, void* edx, int a2) {
  if (!g_trampolineRuntimeModelPlainCtor)
    return nullptr;

  const uintptr_t callerPc = GetCallReturnAddress();
  void* result = g_trampolineRuntimeModelPlainCtor(thisPtr, a2);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelRuntimeCtor);
    RecordRuntimeModelCtor(result ? result : thisPtr, callerPc, false);
  }
  return result;
}

void* __fastcall Hook_RuntimeModelComplexCtor(void* thisPtr, void* edx) {
  if (!g_trampolineRuntimeModelComplexCtor)
    return nullptr;

  const uintptr_t callerPc = GetCallReturnAddress();
  void* result = g_trampolineRuntimeModelComplexCtor(thisPtr);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelRuntimeCtor);
    RecordRuntimeModelCtor(result ? result : thisPtr, callerPc, true);
  }
  return result;
}

void* __stdcall Hook_ResolveRuntimeModelFromHandle(void* handlePtr) {
  if (!g_trampolineResolveRuntimeModelFromHandle)
    return nullptr;

  const uintptr_t callerPc = GetCallReturnAddress();
  void* runtimeModelPtr =
      g_trampolineResolveRuntimeModelFromHandle(handlePtr);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelRuntimeResolve);
    RecordRuntimeModelResolve(runtimeModelPtr, handlePtr, callerPc);
  }
  return runtimeModelPtr;
}

void* __fastcall Hook_PromoteRuntimeModel(void* thisPtr, void* edx) {
  if (!g_trampolinePromoteRuntimeModel)
    return nullptr;

  const uintptr_t callerPc = GetCallReturnAddress();
  void* runtimeModelPtr = g_trampolinePromoteRuntimeModel(thisPtr);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelPromoteRuntime);
    RecordRuntimeModelCreate(runtimeModelPtr, thisPtr, callerPc);
    PublishBuildTimeChildRuntimeModelData(runtimeModelPtr, thisPtr);
  }
  return runtimeModelPtr;
}

char* __fastcall Hook_RuntimeInitFromModelData(char* thisPtr, void* edx,
                                               void* modelDataPtr) {
  if (!g_trampolineRuntimeInitFromModelData)
    return nullptr;

  const uintptr_t callerPc = GetCallReturnAddress();
  char* result = g_trampolineRuntimeInitFromModelData(thisPtr, modelDataPtr);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::ModelHook,
                               render::SemanticDataPerfTag::ModelRuntimeInitCopy);
    RecordRuntimeModelInitCopy(thisPtr, modelDataPtr, callerPc);
  }
  return result;
}

void* __fastcall Hook_BuildChildRuntimeModelLinks(void* thisPtr, void* edx,
                                                  void* modelDataPtr) {
  if (!g_trampolineBuildChildRuntimeModelLinks)
    return nullptr;

  const BuildChildRuntimeScopeState previousScope =
      g_buildChildRuntimeScopeState;
  g_buildChildRuntimeScopeState.depth = previousScope.depth + 1u;
  g_buildChildRuntimeScopeState.parentRuntimeModelPtr = thisPtr;
  g_buildChildRuntimeScopeState.parentModelDataPtr = modelDataPtr;
  std::vector<ModelDataChildLinkProbeRecord> preModelDataLinks;
  if (HasReadableModelDataChildRuntimeLinkHead(modelDataPtr)) {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelBuildChildPreScan);
    RecordBuildTimeModelDataChildLinkScan(thisPtr, modelDataPtr, 1u,
                                          &preModelDataLinks);
  }
  void* result = g_trampolineBuildChildRuntimeModelLinks(thisPtr, modelDataPtr);
  if (g_config.logEnabled) {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::ModelHook,
        render::SemanticDataPerfTag::ModelBuildChildPostScan);
    RecordBuildTimeModelDataChildLinkScan(thisPtr, modelDataPtr, 2u, nullptr);
  }
  g_buildChildRuntimeScopeState = previousScope;
  RecordRuntimeChildLinkBuild(thisPtr, modelDataPtr, &preModelDataLinks);
  return result;
}

int __fastcall Hook_RuntimePoseUpdate(int runtimeModel,
                                      const __m128i *poseMatrix, float scale,
                                      int a4, int a5) {
  if (!g_trampolineRuntimePoseUpdate)
    return 0;

  const int result =
      g_trampolineRuntimePoseUpdate(runtimeModel, poseMatrix, scale, a4, a5);
  {
    SemanticHookPerfScope perf(render::SemanticDataPerfTag::PoseHook,
                               render::SemanticDataPerfTag::PoseRuntimePose);
    RecordRuntimePose(runtimeModel, poseMatrix, scale);
  }
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseRuntimeMatrixPalette);
    uint32_t matrixCount = 0u;
    uint64_t matrixHash = 0u;
    if (RecordRuntimeMatrixPalette(runtimeModel, false, &matrixCount,
                                   &matrixHash)) {
      g_runtimePoseUpdatePalettePublishCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_runtimePoseUpdateLastRuntimeModelPtr.store(
          uint64_t(uint32_t(runtimeModel)), std::memory_order_relaxed);
      g_runtimePoseUpdateLastMatrixCount.store(matrixCount,
                                               std::memory_order_relaxed);
      g_runtimePoseUpdateLastMatrixHash.store(matrixHash,
                                              std::memory_order_relaxed);
    }
  }
  return result;
}

// Phase 7.31 P0（恢复 + 修正）：batch capture 开关。
// Codex 裁决：`0x12E600` 是 CGeosetData_BuildGroupBlendedPalette，按
// `*(CGeosetData + 0xF0)` 的 groupCount 连续写 count * 48 字节到 outPalette。
// Iter F 直接禁用本路径导致 paletteCaptureTrustedSourceMiss ~ 87%
// （live palette 数据 87% 是 arena 残留）。现按 Codex 要求恢复 batch capture，
// 并通过 env flag 提供 A/B 回退入口。
static inline bool RuntimeMatrixBatchCaptureEnabled() {
  static const bool enabled =
      GetEnvBoolCached("DXVK_WAR3_RUNTIME_MATRIX_BATCH_CAPTURE", true);
  return enabled;
}

void __fastcall Hook_RuntimeMatrixWrite(int nodePtr, int sourceMatrixPtr,
                                        int destMatrixPtr) {
  if (!g_trampolineRuntimeMatrixWrite)
    return;

  g_trampolineRuntimeMatrixWrite(nodePtr, sourceMatrixPtr, destMatrixPtr);
  g_runtimeMatrixWriteCount.fetch_add(1u, std::memory_order_relaxed);

  // Phase 7.95：每帧调用次数上限。在桥/斜坡场景下 War3 可能每帧 50K-100K 次
  // matrix write，TryFindRuntimePoseArrayRangeForMatrix 的 shared_lock + hashmap
  // lookup 累积到 40ms+。超过阈值后直接跳过数据层工作。
  // 阈值 20000 足够覆盖正常场景（13K-30K），但能截断极端场景。
  static thread_local uint64_t s_lastFrameTag = 0u;
  static thread_local uint32_t s_frameCallCount = 0u;
  {
    uint32_t currentTag = 0u;
    TryReadCurrentPaletteFrameTag(currentTag);
    if (currentTag != 0u && currentTag != s_lastFrameTag) {
      s_lastFrameTag = currentTag;
      s_frameCallCount = 0u;
    }
  }
  ++s_frameCallCount;
  constexpr uint32_t kMaxCallsPerFrame = 20000u;
  if (s_frameCallCount > kMaxCallsPerFrame)
    return;

  // Phase 7.89：退出地图后 producer 降级为纯透传。g_war3_runtime_activated
  // 在 ResetWar3RuntimeState 时置 false，此后所有 shadow 数据层操作（batch
  // capture / slot cache / frameTag 统计 / PoseRegistry publish）全部跳过，
  // 避免主界面无消费者时仍全速运行导致卡顿。
  if (!dxvk::g_war3_runtime_activated.load(std::memory_order_relaxed))
    return;

  // Phase 7.78：把 frameTag 读取从 dt-gate probe 与 batch-capture 两段
  // 各做一次合并到一次。每帧 13K-30K 次 hook 调用，避免重复读全局 palette
  // frameTag 指针。
  uint32_t frameTagProbe = 0u;
  const bool frameTagOk =
      TryReadCurrentPaletteFrameTag(frameTagProbe) && frameTagProbe != 0u;

  // Phase 7.47 dt gate probe：writer per-frameTag 去重
  if (frameTagOk) {
    NoteWriterHitForFrameTag(g_runtimeMatrixWriteLastFrameTag,
                             g_runtimeMatrixWriteFramesWithHitCount,
                             g_runtimeMatrixWriteFramesEmptyCount,
                             frameTagProbe);
  }

  // Phase 7.31 P0 恢复：按 CGeosetData_BuildGroupBlendedPalette 的真实语义
  // 做 batch capture。关键点：
  //   (1) `count = *(CGeosetData + 0xF0)`，如果为 0 按 1 处理（simple
  //       fallback 路径也走同一 hook）。
  //   (2) 限 count ≤ kRuntimeMatrixBatchMaxCount（与 Query 的 kMaxSlots 对齐）。
  //   (3) 整段 destMatrixPtr + count * 48 可读才进 batch 写入，否则直接放弃。
  //   (4) slot cache 仍是固定数组，O(1) 写入；writeSerial 保证严格递增。
  //
  // 这样 `paletteCaptureTrustedSourceHit` 率可以从 13% 抬到 90%+，让
  // PublishCurrentDrawContract 里的 trusted path 真正吃到 writer-side
  // palette，而不是 arena memcpy 的 86% 残留。
  uintptr_t globalPaletteBuf = ResolveGlobalBlendedPaletteBufferBase();
  if (g_config.enabled && destMatrixPtr != 0 &&
      RuntimeMatrixBatchCaptureEnabled()) {
    const uintptr_t outAddr = uintptr_t(uint32_t(destMatrixPtr));
    if (globalPaletteBuf != 0u && outAddr >= globalPaletteBuf) {
      const uint32_t startSlotIndex =
          uint32_t((outAddr - globalPaletteBuf) / 48u);
      if (startSlotIndex < kSlotBlendedPaletteCacheSize) {
        // 从 CGeosetData+0xF0 读 groupCount；nodePtr 非法时走 simple fallback(1)。
        uint32_t groupCount = 0u;
        bool hasGroupCount = false;
        if (nodePtr != 0) {
          hasGroupCount = SafeReadU32Fast(
              reinterpret_cast<const void*>(uint32_t(nodePtr)),
              0xF0u, groupCount);
        }
        const uint32_t rawCount =
            (hasGroupCount && groupCount != 0u) ? groupCount : 1u;
        g_runtimeMatrixWriteBatchLastGroupCount.store(
            uint64_t(rawCount), std::memory_order_relaxed);
        // Phase 7.78：复用顶部已读到的 frameTag。frameTagOk=false 时
        // 退回到 0（与原行为一致：CaptureBlendedPaletteSlotRange 内部能处理）。
        const uint32_t frameTag = frameTagOk ? frameTagProbe : 0u;
        CaptureBlendedPaletteSlotRange(
            startSlotIndex, reinterpret_cast<const uint8_t*>(outAddr),
            rawCount, frameTag, &g_runtimeMatrixWriteBatchCapturedCount,
            &g_runtimeMatrixWriteBatchOverflowCount,
            &g_runtimeMatrixWriteBatchUnreadableCount);
      }
    }
  }

  RuntimePoseArrayRange range = {};
  uint32_t matrixIndex = 0u;
  if (!TryFindRuntimePoseArrayRangeForMatrix(destMatrixPtr, range,
                                             matrixIndex)) {
    g_runtimeMatrixWriteMissCount.fetch_add(1u, std::memory_order_relaxed);
    return;
  }

  // Publish once the helper reaches the tail of a known CModel+0x60 palette.
  // Earlier writes can be partial; the final slot gives the consumer a coherent
  // palette without re-enabling SpriteFrameUpdate.
  if (matrixIndex + 1u < range.matrixCount)
    return;

  uint32_t matrixCount = 0u;
  uint64_t matrixHash = 0u;
  if (!RecordRuntimeMatrixPalette(int(range.runtimeModel), false, &matrixCount,
                                  &matrixHash)) {
    return;
  }

  g_runtimeMatrixWritePublishCount.fetch_add(1u,
                                             std::memory_order_relaxed);
  g_runtimeMatrixWriteLastRuntimeModelPtr.store(
      range.runtimeModel, std::memory_order_relaxed);
  g_runtimeMatrixWriteLastMatrixIndex.store(matrixIndex,
                                            std::memory_order_relaxed);
  g_runtimeMatrixWriteLastMatrixCount.store(matrixCount,
                                            std::memory_order_relaxed);
  g_runtimeMatrixWriteLastMatrixHash.store(matrixHash,
                                           std::memory_order_relaxed);
}

int __fastcall Hook_RuntimeGroupPaletteWrapper(int runtimeModel,
                                               int poseStackBasePtr) {
  if (!g_trampolineRuntimeGroupPaletteWrapper)
    return 0;

  const int result =
      g_trampolineRuntimeGroupPaletteWrapper(runtimeModel, poseStackBasePtr);
  g_runtimeGroupPaletteWrapperCallCount.fetch_add(1u,
                                                  std::memory_order_relaxed);
  // Phase 7.47 dt gate probe：writer per-frameTag 去重
  {
    uint32_t frameTagProbe = 0u;
    if (TryReadCurrentPaletteFrameTag(frameTagProbe) && frameTagProbe != 0u) {
      NoteWriterHitForFrameTag(g_runtimeGroupPaletteWrapperLastFrameTag,
                               g_runtimeGroupPaletteWrapperFramesWithHitCount,
                               g_runtimeGroupPaletteWrapperFramesEmptyCount,
                               frameTagProbe);
    }
  }
  CaptureRuntimeGroupPaletteBindings(
      runtimeModel, RuntimeGroupPaletteProducerKind::AllocAndFillWrapper,
      false);
  return result;
}

void __fastcall Hook_RuntimeSimpleGroupPalette(int runtimeModel, void* edx) {
  (void)edx;
  if (!g_trampolineRuntimeSimpleGroupPalette)
    return;

  g_trampolineRuntimeSimpleGroupPalette(runtimeModel);
  g_runtimeSimpleGroupPaletteCallCount.fetch_add(1u,
                                                 std::memory_order_relaxed);
  // Phase 7.47 dt gate probe：writer per-frameTag 去重
  {
    uint32_t frameTagProbe = 0u;
    if (TryReadCurrentPaletteFrameTag(frameTagProbe) && frameTagProbe != 0u) {
      NoteWriterHitForFrameTag(g_runtimeSimpleGroupPaletteLastFrameTag,
                               g_runtimeSimpleGroupPaletteFramesWithHitCount,
                               g_runtimeSimpleGroupPaletteFramesEmptyCount,
                               frameTagProbe);
    }
  }
  CaptureRuntimeGroupPaletteBindings(
      runtimeModel, RuntimeGroupPaletteProducerKind::SimpleFallback, true);
}

int __fastcall Hook_RuntimePropagatePoseTree(int runtimeModel, int a2) {
  if (!g_trampolineRuntimePropagatePoseTree)
    return 0;

  const int result = g_trampolineRuntimePropagatePoseTree(runtimeModel, a2);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseRuntimeMatrixPalette);
    RecordRuntimeMatrixPalette(runtimeModel, false);
  }
  return result;
}

void __fastcall Hook_RuntimeRecurseChildTree(int runtimeModel, int a2) {
  if (!g_trampolineRuntimeRecurseChildTree)
    return;

  g_trampolineRuntimeRecurseChildTree(runtimeModel, a2);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseRuntimeMatrixPalette);
    RecordRuntimeMatrixPalette(runtimeModel, false);
  }
}

int __fastcall Hook_SpriteFrameUpdate(int thisPtr, void* edx, float dt, int a3,
                                      unsigned int a4, int a5) {
  if (!g_trampolineSpriteFrameUpdate)
    return 0;

  // Phase 7.47 dt gate probe：trampoline 前记录 dt（包括早退路径）。
  NoteSpriteUberPreRenderDtBucket(dt);

  const uintptr_t callerPc = GetCallReturnAddress();
  const int result =
      g_trampolineSpriteFrameUpdate(thisPtr, dt, a3, a4, a5);
  // probe-only 模式下跳过 identity/pose 重路径，只保留 dt 统计。
  if (!g_config.poseEnabled && SpriteUberDtProbeEnabled()) {
    return result;
  }
  if (g_config.poseEnabled) {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseSpriteFrameSourceIdentity);
    MaybeRecordSpriteFrameSourceIdentity(thisPtr, a3);
  }
  RecordSpriteFramePoseFromSprite(
      thisPtr, dt, reinterpret_cast<void*>(uintptr_t(uint32_t(a3))),
      kSpriteFrameUpdateKindFull, callerPc);
  return result;
}

int __fastcall Hook_SpriteMiniFrameUpdate(int thisPtr, void* edx, float dt,
                                          int a3, unsigned int a4, int a5) {
  if (!g_trampolineSpriteMiniFrameUpdate)
    return 0;

  // Phase 7.47 dt gate probe
  NoteSpriteUberPreRenderDtBucket(dt);

  const uintptr_t callerPc = GetCallReturnAddress();
  const int result =
      g_trampolineSpriteMiniFrameUpdate(thisPtr, dt, a3, a4, a5);
  if (!g_config.poseEnabled && SpriteUberDtProbeEnabled()) {
    return result;
  }
  if (g_config.poseEnabled) {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseSpriteFrameSourceIdentity);
    MaybeRecordSpriteFrameSourceIdentity(thisPtr, a3);
  }
  RecordSpriteFramePoseFromSprite(
      thisPtr, dt, reinterpret_cast<void*>(uintptr_t(uint32_t(a3))),
      kSpriteFrameUpdateKindMini, callerPc);
  return result;
}

int __fastcall Hook_SpriteFrameLiteUpdate(int thisPtr, void* edx, float dt) {
  if (!g_trampolineSpriteFrameLiteUpdate)
    return 0;

  // Phase 7.47 dt gate probe
  NoteSpriteUberPreRenderDtBucket(dt);

  const uintptr_t callerPc = GetCallReturnAddress();
  const int result = g_trampolineSpriteFrameLiteUpdate(thisPtr, dt);
  if (!g_config.poseEnabled && SpriteUberDtProbeEnabled()) {
    return result;
  }
  RecordSpriteFramePoseFromSprite(thisPtr, dt, nullptr,
                                  kSpriteFrameUpdateKindLite, callerPc);
  return result;
}

int __fastcall Hook_SpriteMiniFrameLiteUpdate(int thisPtr, void* edx,
                                              float dt) {
  if (!g_trampolineSpriteMiniFrameLiteUpdate)
    return 0;

  // Phase 7.47 dt gate probe
  NoteSpriteUberPreRenderDtBucket(dt);

  const uintptr_t callerPc = GetCallReturnAddress();
  const int result = g_trampolineSpriteMiniFrameLiteUpdate(thisPtr, dt);
  if (!g_config.poseEnabled && SpriteUberDtProbeEnabled()) {
    return result;
  }
  RecordSpriteFramePoseFromSprite(thisPtr, dt, nullptr,
                                  kSpriteFrameUpdateKindMiniLite, callerPc);
  return result;
}

int __fastcall Hook_RuntimeMatrixRangeCopy(int runtimeModel, int a2, int a3) {
  if (!g_trampolineRuntimeMatrixRangeCopy)
    return 0;

  const int result = g_trampolineRuntimeMatrixRangeCopy(runtimeModel, a2, a3);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseRuntimeMatrixPublisher);
    RecordRuntimeMatrixPublisher(runtimeModel, 1u);
  }
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseRuntimeMatrixPalette);
    constexpr bool preferRuntimePoseUpdate =
        dxvk::war3::internal::
            kWar3RuntimeConfigSemanticRuntimePoseUpdateEffective &&
        dxvk::war3::internal::
            kWar3RuntimeConfigPreferSemanticRuntimePoseUpdatePalette;
    // 0x6F12FDC0 is the authoritative animated matrix range-copy. Read the
    // exact source segment from its arguments before any later runtime flush can
    // replace CModel+0x60 with the shared reset matrix.
    //
    // Phase 7.34 A3: 激活 range-copy 作为 authoritative pose publisher。
    // 历史上两个分支都传了 publishPalette=false，仅做诊断统计，导致 0x12FDC0
    // 的权威 final-pose 数据从未写入 PoseRegistry。严格仲裁（A2）开启后，
    // trusted miss 直接丢弃 publish，视角移动/渲染压力大时对象阴影会一帧消失
    // 产生"闪烁"。A3 把默认值改成 true，让 0x12FDC0 的完整 final-pose palette
    // 被发布为 PoseRegistry 的 matrixPalette，作为 trusted cache 的对账 oracle。
    //
    // 回滚开关：`DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH=0` 可恢复旧行为
    //（仅写诊断 counter，不发布 palette）。
    static const bool s_publishRangeCopyPalette =
        GetEnvBoolCached("DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH", true);
    if (preferRuntimePoseUpdate) {
      // preferRuntimePoseUpdate 路径：RuntimePoseUpdate 会在更晚的时机发布
      // 一段 stable matrix segment。为避免 range-copy 覆盖它的结果，仍保持
      // publishPalette=false（以该分支的 s_publishRangeCopyPalette 为 false
      // 更保守，即使 A3 开关打开也不要冲突）。
      RecordRuntimeMatrixPaletteFromRangeCopy(runtimeModel, a2, a3, false,
                                              false);
    } else if (!RecordRuntimeMatrixPaletteFromRangeCopy(
                   runtimeModel, a2, a3, false, s_publishRangeCopyPalette) &&
               RecordRuntimeMatrixPalette(runtimeModel, false)) {
      g_runtimeMatrixRangeCopyPaletteFallbackCModelCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
  return result;
}

int __fastcall Hook_RuntimeMatrixFlush(int runtimeModel, void* edx) {
  if (!g_trampolineRuntimeMatrixFlush)
    return 0;

  const int result = g_trampolineRuntimeMatrixFlush(runtimeModel);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::PoseHook,
        render::SemanticDataPerfTag::PoseRuntimeMatrixPublisher);
    RecordRuntimeMatrixPublisher(runtimeModel, 2u);
  }
  g_runtimeMatrixFlushPaletteSuppressedCount.fetch_add(
      1u, std::memory_order_relaxed);
  // 0x6F12FF50 is the runtime matrix flush/reset path: it fills CModel+0x60
  // from the shared global matrix, so publishing it as a pose can overwrite the
  // animated range-copy palette with bind/initial posture. Keep it diagnostic
  // only; the authoritative palette producer is 0x6F12FDC0.
  return result;
}

const __m128i* __fastcall Hook_RuntimeWriteSharedPresetOutput(int contextPtr,
                                                              int nodePtr) {
  if (!g_trampolineRuntimeWriteSharedPresetOutput)
    return nullptr;

  const __m128i* result =
      g_trampolineRuntimeWriteSharedPresetOutput(contextPtr, nodePtr);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::AttachmentHook,
        render::SemanticDataPerfTag::AttachmentOverrideSharedPreset);
    RecordOverrideSharedPresetWrite(contextPtr, nodePtr);
  }
  return result;
}

int __fastcall Hook_RuntimeWriteLocalPointOutput(int contextPtr, int nodePtr) {
  if (!g_trampolineRuntimeWriteLocalPointOutput)
    return 0;

  const int result =
      g_trampolineRuntimeWriteLocalPointOutput(contextPtr, nodePtr);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::AttachmentHook,
        render::SemanticDataPerfTag::AttachmentOverrideLocalPoint);
    RecordOverrideLocalPointWrite(contextPtr, nodePtr);
  }
  return result;
}

const __m128i* __fastcall Hook_RuntimeWritePrimaryPresetOutput(int contextPtr,
                                                               int nodePtr,
                                                               float* a3) {
  if (!g_trampolineRuntimeWritePrimaryPresetOutput)
    return nullptr;

  const __m128i* result = g_trampolineRuntimeWritePrimaryPresetOutput(
      contextPtr, nodePtr, a3);
  {
    SemanticHookPerfScope perf(
        render::SemanticDataPerfTag::AttachmentHook,
        render::SemanticDataPerfTag::AttachmentOverridePrimaryPreset);
    RecordOverridePrimaryPresetWrite(contextPtr, nodePtr);
  }
  return result;
}

bool InstallCreateSpriteAndBindSourceObjectHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kCreateSpriteAndBindSourceObjectRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite host bind 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_CreateSpriteAndBindSourceObject),
      reinterpret_cast<LPVOID*>(&g_trampolineCreateSpriteAndBindSourceObject),
      "Model", "CreateSpriteAndBindSourceObject", true, g_config.logEnabled);
}

bool InstallAttachedEffectInitHook(uintptr_t gameBase) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return false;
  }
  const uintptr_t target = gameBase + kAttachedEffectInitRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: attached effect init 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_AttachedEffectInit),
      reinterpret_cast<LPVOID*>(&g_trampolineAttachedEffectInit), "Model",
      "AttachedEffectInit", true, g_config.logEnabled);
}

bool InstallAttachedEffectDirectAttachHook(uintptr_t gameBase) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return false;
  }
  const uintptr_t target = gameBase + kAttachedEffectDirectAttachRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: attached effect direct attach 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_AttachedEffectDirectAttach),
      reinterpret_cast<LPVOID*>(&g_trampolineAttachedEffectDirectAttach),
      "Model", "AttachedEffectDirectAttach", true, g_config.logEnabled);
}

bool InstallAttachModelToPointHook(uintptr_t gameBase) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    return false;
  }
  const uintptr_t target = gameBase + kAttachModelToPointRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: attach model to point 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_AttachModelToPoint),
      reinterpret_cast<LPVOID*>(&g_trampolineAttachModelToPoint), "Model",
      "AttachModelToPoint", true, g_config.logEnabled);
}

bool InstallCreateSpriteRuntimeHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kCreateSpriteRuntimeRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime sprite ctor 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_CreateSpriteRuntime),
      reinterpret_cast<LPVOID *>(&g_trampolineCreateSpriteRuntime), "Model",
      "CreateSpriteRuntime", true, g_config.logEnabled);
}

bool InstallCreateGeosetFromRawArraysHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kCreateGeosetFromRawArraysRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: geoset create 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_CreateGeosetFromRawArrays),
      reinterpret_cast<LPVOID *>(&g_trampolineCreateGeosetFromRawArrays),
      "Model", "CreateGeosetFromRawArrays", true, g_config.logEnabled);
}

bool InstallRuntimeModelPlainCtorHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeModelPlainCtorRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime model plain ctor 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeModelPlainCtor),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeModelPlainCtor), "Model",
      "RuntimeModelPlainCtor", true, g_config.logEnabled);
}

bool InstallRuntimeModelComplexCtorHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeModelComplexCtorRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime model complex ctor 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeModelComplexCtor),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeModelComplexCtor), "Model",
      "RuntimeModelComplexCtor", true, g_config.logEnabled);
}

bool InstallResolveRuntimeModelFromHandleHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kResolveRuntimeModelFromHandleRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: resolve runtime model from handle 不可执行，跳过 Hook "
        "(addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_ResolveRuntimeModelFromHandle),
      reinterpret_cast<LPVOID*>(&g_trampolineResolveRuntimeModelFromHandle),
      "Model", "ResolveRuntimeModelFromHandle", true, g_config.logEnabled);
}

bool InstallPromoteRuntimeModelHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kPromoteRuntimeModelRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: promote runtime model 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_PromoteRuntimeModel),
      reinterpret_cast<LPVOID*>(&g_trampolinePromoteRuntimeModel), "Model",
      "PromoteRuntimeModel", true, g_config.logEnabled);
}

bool InstallRuntimeInitFromModelDataHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeInitFromModelDataRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime init from model data 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeInitFromModelData),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeInitFromModelData),
      "Model", "RuntimeInitFromModelData", true, g_config.logEnabled);
}

bool InstallBuildChildRuntimeModelLinksHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kBuildChildRuntimeModelLinksRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: build child runtime links 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_BuildChildRuntimeModelLinks),
      reinterpret_cast<LPVOID*>(&g_trampolineBuildChildRuntimeModelLinks),
      "Model", "BuildChildRuntimeModelLinks", true, g_config.logEnabled);
}

bool InstallSpriteFrameUpdateHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kSpriteFrameUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite frame update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_SpriteFrameUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineSpriteFrameUpdate), "Model",
      "SpriteFrameUpdate", true, g_config.logEnabled);
}

bool InstallSpriteMiniFrameUpdateHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kSpriteMiniFrameUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite mini frame update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_SpriteMiniFrameUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineSpriteMiniFrameUpdate), "Model",
      "SpriteMiniFrameUpdate", true, g_config.logEnabled);
}

bool InstallSpriteFrameLiteUpdateHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kSpriteFrameLiteUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite frame lite update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_SpriteFrameLiteUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineSpriteFrameLiteUpdate), "Model",
      "SpriteFrameLiteUpdate", true, g_config.logEnabled);
}

bool InstallSpriteMiniFrameLiteUpdateHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kSpriteMiniFrameLiteUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite mini frame lite update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_SpriteMiniFrameLiteUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineSpriteMiniFrameLiteUpdate),
      "Model", "SpriteMiniFrameLiteUpdate", true, g_config.logEnabled);
}

bool InstallRuntimePoseHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimePoseUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime pose update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimePoseUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineRuntimePoseUpdate), "Model",
      "RuntimePoseUpdate", true, g_config.logEnabled);
}

bool InstallRuntimeMatrixWriteHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeMatrixWriteRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime matrix write 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeMatrixWrite),
      reinterpret_cast<LPVOID *>(&g_trampolineRuntimeMatrixWrite), "Model",
      "RuntimeMatrixWrite", true, g_config.logEnabled);
}

bool InstallRuntimeGroupPaletteWrapperHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeGroupPaletteWrapperRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime group palette wrapper 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeGroupPaletteWrapper),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeGroupPaletteWrapper),
      "Model", "RuntimeGroupPaletteWrapper", true, g_config.logEnabled);
}

bool InstallRuntimeSimpleGroupPaletteHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeSimpleGroupPaletteRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime simple group palette 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeSimpleGroupPalette),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeSimpleGroupPalette),
      "Model", "RuntimeSimpleGroupPalette", true, g_config.logEnabled);
}

bool InstallRuntimePropagatePoseTreeHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimePropagatePoseTreeRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime propagate pose tree 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimePropagatePoseTree),
      reinterpret_cast<LPVOID *>(&g_trampolineRuntimePropagatePoseTree),
      "Model", "RuntimePropagatePoseTree", true, g_config.logEnabled);
}

bool InstallRuntimeRecurseChildTreeHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeRecurseChildTreeRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime recurse child tree 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeRecurseChildTree),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeRecurseChildTree), "Model",
      "RuntimeRecurseChildTree", true, g_config.logEnabled);
}

bool InstallRuntimeMatrixRangeCopyHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeMatrixRangeCopyRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime matrix range copy 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeMatrixRangeCopy),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeMatrixRangeCopy), "Model",
      "RuntimeMatrixRangeCopy", true, g_config.logEnabled);
}

bool InstallRuntimeMatrixFlushHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeMatrixFlushRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime matrix flush 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeMatrixFlush),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeMatrixFlush), "Model",
      "RuntimeMatrixFlush", true, g_config.logEnabled);
}

bool InstallRuntimeWriteSharedPresetOutputHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeWriteSharedPresetOutputRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime shared preset output 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeWriteSharedPresetOutput),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeWriteSharedPresetOutput),
      "Model", "RuntimeWriteSharedPresetOutput", true, g_config.logEnabled);
}

bool InstallRuntimeWriteLocalPointOutputHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeWriteLocalPointOutputRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime local point output 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeWriteLocalPointOutput),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeWriteLocalPointOutput),
      "Model", "RuntimeWriteLocalPointOutput", true, g_config.logEnabled);
}

bool InstallRuntimeWritePrimaryPresetOutputHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeWritePrimaryPresetOutputRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime primary preset output 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeWritePrimaryPresetOutput),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeWritePrimaryPresetOutput),
      "Model", "RuntimeWritePrimaryPresetOutput", true, g_config.logEnabled);
}
} // namespace

// 通过 slotIndex 查询 Hook_RuntimeMatrixWrite 当场捕获的混合调色板
bool QueryBlendedPaletteBySlotIndex(uint32_t slotIndex,
                                     void* outPaletteVec,
                                     uint32_t& outGroupCount) {
  auto& outPalette = *reinterpret_cast<std::vector<Matrix4>*>(outPaletteVec);
  outPalette.clear();
  outGroupCount = 0u;
  uint32_t currentFrameTag = 0u;
  const bool hasCurrentFrameTag = TryReadCurrentPaletteFrameTag(currentFrameTag);
  uint32_t expectedFrameTag = 0u;
  uint64_t previousWriteSerial = 0u;
  // 每次写只存了 1 个矩阵，需要遍历连续槽位收集整个 geoset 的调色板
  constexpr uint32_t kMaxSlots = 256u;
  for (uint32_t s = slotIndex; s < slotIndex + kMaxSlots; ++s) {
    if (s >= kSlotBlendedPaletteCacheSize)
      break;
    const auto& entry = s_slotBlendedPaletteCache[s];
    if (!entry.valid)
      break;
    if (expectedFrameTag == 0u) {
      expectedFrameTag = entry.frameTag;
      if (hasCurrentFrameTag && expectedFrameTag != currentFrameTag)
        return false;
    } else if (entry.frameTag != expectedFrameTag) {
      break;
    }
    if (previousWriteSerial != 0u && entry.writeSerial <= previousWriteSerial)
      break;
    previousWriteSerial = entry.writeSerial;
    outPalette.push_back(entry.matrix);
  }
  if (outPalette.empty())
    return false;
  outGroupCount = uint32_t(outPalette.size());
  return true;
}

// Phase 7.30 Action B 第二刀：按 frameTag 校验的精确 query。
// Phase 7.34 诊断 counter：用于观察 exact/bestEffort 拒绝分布。
std::atomic<uint64_t> g_queryBlendedPaletteExactHitCount{0u};
std::atomic<uint64_t> g_queryBlendedPaletteRejectedSlotOverflowCount{0u};
std::atomic<uint64_t> g_queryBlendedPaletteRejectedInvalidEntryCount{0u};
std::atomic<uint64_t> g_queryBlendedPaletteRejectedFrameTagMismatchCount{0u};
std::atomic<uint64_t> g_queryBlendedPaletteRejectedShortResultCount{0u};
std::atomic<uint64_t> g_queryBlendedPaletteBestEffortHitCount{0u};

// 只要 slotIndex..slotIndex+count-1 每个 entry 的 frameTag 都与当前帧
// (或请求的 expectedFrameTag) 一致，就返回 palette。不要求 writeSerial
// 单调递增——引擎的 matrix-write 顺序与 slot 顺序不一定吻合，只要 per-slot
// 最近一次写就在本帧就是可信的。
//
// Phase 7.34 重写：恢复 "Exact" 真正的严格语义。
// 历史背景：Phase 1 曾把 break 当成 return 部分命中，结果是
//   `size < expectedCount` 的调用端用零填充后续矩阵 → skinned caster 阴影
//   数据污染，是用户观察到的"只显示一个部位 + 有闪烁"的直接原因之一。
//
// 新语义：
//   - Exact 版本：所有 expectedCount 个 slot 必须 valid 且 frameTag 同帧
//     （允许差 1 帧，因为骨骼计算先于 draw）；任何一项失败即 return false，
//     outPalette 清空。
//   - BestEffort 版本：保留旧宽松行为，**仅供诊断**，不应参与 ready 仲裁。
//
// 调用端必须检查 `outPalette.size() == expectedCount`，否则视为 miss。
bool QueryBlendedPaletteBySlotIndexExact(uint32_t slotIndex,
                                         uint32_t expectedCount,
                                         uint32_t expectedFrameTag,
                                         void* outPaletteVec) {
  auto& outPalette = *reinterpret_cast<std::vector<Matrix4>*>(outPaletteVec);
  outPalette.clear();
  if (expectedCount == 0u || expectedCount > 256u)
    return false;
  if (slotIndex + expectedCount > kSlotBlendedPaletteCacheSize) {
    g_queryBlendedPaletteRejectedSlotOverflowCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  outPalette.reserve(expectedCount);
  for (uint32_t i = 0u; i < expectedCount; ++i) {
    const uint32_t s = slotIndex + i;
    const auto& entry = s_slotBlendedPaletteCache[s];
    if (!entry.valid) {
      g_queryBlendedPaletteRejectedInvalidEntryCount.fetch_add(
          1u, std::memory_order_relaxed);
      outPalette.clear();
      return false;
    }
    // Phase 7.35 路径 1：原先只允许差 1 帧，但相机移动时骨骼计算 0x12E600
    // 通常是 pre-pass 先于当帧 draw（observed delta=2 很普遍），24.4% miss 就是
    // 这一刀切掉的。放宽到 2 帧（与 capture serial diff<=2 保持对齐），压下
    // `paletteCaptureFrameTagMismatchMissCount`。不往 3+ 放宽：那样骨骼延迟视觉
    // 可感知，属于 path 2 该解决的问题。
    if (expectedFrameTag != 0u && entry.frameTag != 0u) {
      const uint32_t delta = (expectedFrameTag >= entry.frameTag)
                                 ? (expectedFrameTag - entry.frameTag)
                                 : (entry.frameTag - expectedFrameTag);
      if (delta > 2u) {
        g_queryBlendedPaletteRejectedFrameTagMismatchCount.fetch_add(
            1u, std::memory_order_relaxed);
        outPalette.clear();
        return false;
      }
    }
    outPalette.push_back(entry.matrix);
  }
  // 最终完整性复查：任何 partial 情形统一判 fail。
  if (outPalette.size() != expectedCount) {
    g_queryBlendedPaletteRejectedShortResultCount.fetch_add(
        1u, std::memory_order_relaxed);
    outPalette.clear();
    return false;
  }
  g_queryBlendedPaletteExactHitCount.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

// Phase 7.34：诊断用的 best-effort 查询，允许 partial 返回。
// **不应用于 Ready palette 仲裁**，仅在 counter / 调试日志中使用。
bool QueryBlendedPaletteBySlotIndexBestEffort(uint32_t slotIndex,
                                              uint32_t expectedCount,
                                              uint32_t expectedFrameTag,
                                              void* outPaletteVec) {
  auto& outPalette = *reinterpret_cast<std::vector<Matrix4>*>(outPaletteVec);
  outPalette.clear();
  if (expectedCount == 0u || expectedCount > 256u)
    return false;
  if (slotIndex + expectedCount > kSlotBlendedPaletteCacheSize)
    return false;
  outPalette.reserve(expectedCount);
  for (uint32_t i = 0u; i < expectedCount; ++i) {
    const uint32_t s = slotIndex + i;
    const auto& entry = s_slotBlendedPaletteCache[s];
    if (!entry.valid)
      break;
    if (expectedFrameTag != 0u && entry.frameTag != 0u) {
      const uint32_t delta = (expectedFrameTag >= entry.frameTag)
                                 ? (expectedFrameTag - entry.frameTag)
                                 : (entry.frameTag - expectedFrameTag);
      if (delta > 1u)
        break;
    }
    outPalette.push_back(entry.matrix);
  }
  if (!outPalette.empty())
    g_queryBlendedPaletteBestEffortHitCount.fetch_add(
        1u, std::memory_order_relaxed);
  return !outPalette.empty();
}

bool QueryCurrentPaletteFrameTag(uint32_t& outFrameTag) {
  return TryReadCurrentPaletteFrameTag(outFrameTag);
}

bool QueryBlendedPaletteFrameTagRange(uint32_t slotIndex,
                                      uint32_t expectedCount,
                                      uint32_t& outMinFrameTag,
                                      uint32_t& outMaxFrameTag,
                                      uint32_t& outMissingCount) {
  outMinFrameTag = 0u;
  outMaxFrameTag = 0u;
  outMissingCount = 0u;
  if (expectedCount == 0u || expectedCount > 256u) {
    outMissingCount = expectedCount;
    return false;
  }
  if (slotIndex + expectedCount > kSlotBlendedPaletteCacheSize) {
    outMissingCount = expectedCount;
    return false;
  }

  bool hasAnyFrameTag = false;
  for (uint32_t i = 0u; i < expectedCount; ++i) {
    const auto& entry = s_slotBlendedPaletteCache[slotIndex + i];
    if (!entry.valid || entry.frameTag == 0u) {
      outMissingCount++;
      continue;
    }

    const uint32_t tag = entry.frameTag;
    if (!hasAnyFrameTag) {
      outMinFrameTag = tag;
      outMaxFrameTag = tag;
      hasAnyFrameTag = true;
    } else {
      outMinFrameTag = std::min(outMinFrameTag, tag);
      outMaxFrameTag = std::max(outMaxFrameTag, tag);
    }
  }

  return hasAnyFrameTag && outMissingCount == 0u;
}

bool QueryRenderablePartPaletteSlot(void* renderablePart,
                                    uint32_t& outSlotIndex,
                                    uint32_t* outGroupCount,
                                    uint32_t* outFrameTag) {
  outSlotIndex = 0xFFFFFFFFu;
  if (outGroupCount != nullptr)
    *outGroupCount = 0u;
  if (outFrameTag != nullptr)
    *outFrameTag = 0u;
  if (renderablePart == nullptr) {
    g_renderablePartPaletteBindingQueryMissCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  const uintptr_t partValue = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      (partValue >> 4u) % kRenderablePartPaletteBindingCacheSize;
  const auto& entry = s_renderablePartPaletteBindings[slot];
  if (entry.renderablePart.load(std::memory_order_acquire) != partValue) {
    g_renderablePartPaletteBindingQueryMissCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  const uint32_t slotIndex =
      entry.paletteSlotIndex.load(std::memory_order_relaxed);
  if (slotIndex == 0xFFFFFFFFu || slotIndex >= 0x3A98u) {
    g_renderablePartPaletteBindingQueryMissCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  outSlotIndex = slotIndex;
  if (outGroupCount != nullptr)
    *outGroupCount = entry.groupCount.load(std::memory_order_relaxed);
  if (outFrameTag != nullptr)
    *outFrameTag = entry.frameTag.load(std::memory_order_relaxed);
  g_renderablePartPaletteBindingQueryHitCount.fetch_add(
      1u, std::memory_order_relaxed);
  return true;
}

bool QueryRenderablePartPaletteSnapshot(void* renderablePart,
                                        uint32_t expectedCount,
                                        void* outPaletteVec,
                                        uint64_t* outHash,
                                        uint32_t* outFrameTag) {
  auto& outPalette = *reinterpret_cast<std::vector<Matrix4>*>(outPaletteVec);
  outPalette.clear();
  if (outHash != nullptr)
    *outHash = 0u;
  if (outFrameTag != nullptr)
    *outFrameTag = 0u;

  auto noteMiss = [&]() {
    g_renderablePartPaletteSnapshotQueryMissCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  };

  if (!RenderablePartPaletteSnapshotEnabled() || renderablePart == nullptr ||
      expectedCount == 0u ||
      expectedCount > kRenderablePartPaletteSnapshotMaxCount) {
    return noteMiss();
  }

  const uintptr_t partValue = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      (partValue >> 4u) % kRenderablePartPaletteBindingCacheSize;
  const auto& entry = s_renderablePartPaletteBindings[slot];
  if (entry.renderablePart.load(std::memory_order_acquire) != partValue)
    return noteMiss();

  const uint64_t serialBefore =
      entry.paletteWriteSerial.load(std::memory_order_acquire);
  if (serialBefore == 0u || (serialBefore & 1u) != 0u)
    return noteMiss();

  const uint32_t paletteCount =
      entry.paletteCount.load(std::memory_order_relaxed);
  if (paletteCount < expectedCount ||
      paletteCount > kRenderablePartPaletteSnapshotMaxCount) {
    return noteMiss();
  }

  outPalette.resize(expectedCount);
  for (uint32_t i = 0u; i < expectedCount; ++i)
    outPalette[i] = entry.palette[i];

  const uint64_t serialAfter =
      entry.paletteWriteSerial.load(std::memory_order_acquire);
  if (serialBefore != serialAfter || (serialAfter & 1u) != 0u) {
    outPalette.clear();
    return noteMiss();
  }

  const uint32_t frameTag =
      entry.paletteFrameTag.load(std::memory_order_relaxed);
  uint64_t paletteHash = 0u;
  if (paletteCount == expectedCount) {
    paletteHash = entry.paletteHash.load(std::memory_order_relaxed);
  }
  if (paletteHash == 0u)
    paletteHash = HashMatrixPalette(outPalette);

  if (outHash != nullptr)
    *outHash = paletteHash;
  if (outFrameTag != nullptr)
    *outFrameTag = frameTag;
  g_renderablePartPaletteSnapshotQueryHitCount.fetch_add(
      1u, std::memory_order_relaxed);
  return true;
}

// Phase 7.51：通过 renderablePart 反查 producer 侧 runtimeModel 指针。
// 当 0x12FED0/0x12FF90 producer 触发时，我们记了 (renderablePart, runtimeModel)
// 绑定；submit 时 packet.renderable.runtimeModelPtr 经常是经过 alias 解析的别名
// 值，PoseRegistry miss，而这里返回的才是 PoseRegistry 的原始 key。
bool QueryRenderablePartOwnerRuntimeModel(void* renderablePart,
                                          void** outRuntimeModelPtr) {
  if (outRuntimeModelPtr != nullptr)
    *outRuntimeModelPtr = nullptr;
  if (renderablePart == nullptr || outRuntimeModelPtr == nullptr)
    return false;

  const uintptr_t partValue = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      (partValue >> 4u) % kRenderablePartPaletteBindingCacheSize;
  const auto& entry = s_renderablePartPaletteBindings[slot];
  if (entry.renderablePart.load(std::memory_order_acquire) != partValue)
    return false;

  const uintptr_t runtimeValue =
      entry.runtimeModel.load(std::memory_order_relaxed);
  if (runtimeValue == 0u)
    return false;

  *outRuntimeModelPtr = reinterpret_cast<void*>(runtimeValue);
  return true;
}

bool TryBootstrapRuntimeChildLineageFromParentModelData(
    void* parentRuntimeModelPtr, void* parentModelDataPtr,
    void* childRuntimeModelPtr, uint32_t sourceMeta, uint32_t bucketIndex,
    void*& outChildModelDataPtr, void*& outChildModelResourcePtr) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticAttachmentProducerEffective) {
    outChildModelDataPtr = nullptr;
    outChildModelResourcePtr = nullptr;
    return false;
  }
  if (!g_config.poseEnabled || !g_config.attachmentEnabled) {
    outChildModelDataPtr = nullptr;
    outChildModelResourcePtr = nullptr;
    return false;
  }
  return InternalTryBootstrapRuntimeChildLineageFromParentModelData(
      parentRuntimeModelPtr, parentModelDataPtr, childRuntimeModelPtr,
      sourceMeta, bucketIndex, outChildModelDataPtr, outChildModelResourcePtr,
      nullptr);
}

void Init(uintptr_t gameBase, bool bootstrapOnly) {
  const bool semanticDataEnabled =
      dxvk::war3::runtime::IsWar3RuntimeModuleEnabled(
          dxvk::war3::runtime::War3RuntimeModule::SemanticData);
  g_config.enabled =
      semanticDataEnabled &&
      dxvk::war3::internal::
          kWar3RuntimeConfigSemanticModelProducerEffective &&
      GetEnvBoolCached("DXVK_WAR3_MODEL_HOOK",
                       dxvk::war3::internal::kShadowRuntimeModelHookEnabled);
  g_config.logEnabled = GetEnvBoolCached("DXVK_WAR3_MODEL_LOG", false);
  // 上层语义阴影 consumer 已经开始直接消费 runtime palette，因此这里不再
  // 保持“默认关闭”。若需要快速止血，仍可通过环境变量显式关闭：
  //   DXVK_WAR3_MODEL_POSE_HOOK=0
  g_config.poseEnabled =
      g_config.enabled &&
      dxvk::war3::internal::
          kWar3RuntimeConfigSemanticPoseProducerEffective &&
      GetEnvBoolCached("DXVK_WAR3_MODEL_POSE_HOOK",
                       dxvk::war3::internal::kShadowRuntimePoseHookEnabled);
  g_config.attachmentEnabled =
      g_config.enabled &&
      dxvk::war3::internal::
          kWar3RuntimeConfigSemanticAttachmentProducerEffective;
  const bool matrixPublisherPoseEnabled =
      g_config.enabled &&
      dxvk::war3::internal::
          kWar3RuntimeConfigSemanticMatrixPublisherPoseEffective;
  const bool runtimePoseUpdateEnabled =
      g_config.enabled &&
      dxvk::war3::internal::
          kWar3RuntimeConfigSemanticRuntimePoseUpdateEffective;
  const bool runtimeMatrixWriteEnabled =
      g_config.enabled &&
      dxvk::war3::internal::
          kWar3RuntimeConfigSemanticRuntimeMatrixWriteEffective;

  war3dbg::Print(
      "DXVK_Model: init enabled=%d pose=%d runtimePoseUpdate=%d "
      "matrixWrite=%d matrixPublisherPose=%d attach=%d log=%d semanticData=%d\n",
                 g_config.enabled ? 1 : 0, g_config.poseEnabled ? 1 : 0,
                 runtimePoseUpdateEnabled ? 1 : 0,
                 runtimeMatrixWriteEnabled ? 1 : 0,
                 matrixPublisherPoseEnabled ? 1 : 0,
                 g_config.attachmentEnabled ? 1 : 0,
                 g_config.logEnabled ? 1 : 0,
                 semanticDataEnabled ? 1 : 0);

  if (!g_config.enabled || !gameBase)
    return;

  if (bootstrapOnly &&
      g_bootstrapHooksInstalled.load(std::memory_order_relaxed)) {
    return;
  }
  if (!bootstrapOnly && g_fullHooksInstalled.load(std::memory_order_relaxed))
    return;

  g_gameBase = gameBase;
  const bool firstInstallRound =
      !g_bootstrapHooksInstalled.load(std::memory_order_relaxed) &&
      !g_fullHooksInstalled.load(std::memory_order_relaxed);
  if (firstInstallRound) {
    g_spriteHostBindCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelCtorCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelComplexCtorCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelPlainCtorCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelCtorCallerPromoteCount.store(0u,
                                               std::memory_order_relaxed);
    g_runtimeModelCtorCallerOtherCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelCreateCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelResolveCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelResolveResolvedIdentityCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeModelCreateCallerBuildChildLinksCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeModelCreateCallerCreateSpriteRuntimeCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeModelCreateCallerOtherCount.store(0u,
                                               std::memory_order_relaxed);
    g_runtimeModelInitCopyCount.store(0u, std::memory_order_relaxed);
    g_runtimeModelInitCopyPublishedFallbackCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeChildLinkBuildCount.store(0u, std::memory_order_relaxed);
    g_runtimeChildLinkBuiltChildCount.store(0u, std::memory_order_relaxed);
    g_runtimeChildBuildTimeDirectPublishCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeChildBuildTimeDirectPublishWithResourceCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeChildBuildModelDataPreLinkCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeChildBuildModelDataPostLinkCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeChildBuildModelDataPreUnreadableLinkCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeChildBuildModelDataPostUnreadableLinkCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyCount.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixFlushCount.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherPaletteReadyCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherPoseRevision.store(0u,
                                               std::memory_order_relaxed);
    g_runtimePoseUpdatePalettePublishCount.store(
        0u, std::memory_order_relaxed);
    g_runtimePoseUpdateLastRuntimeModelPtr.store(
        0u, std::memory_order_relaxed);
    g_runtimePoseUpdateLastMatrixCount.store(0u,
                                             std::memory_order_relaxed);
    g_runtimePoseUpdateLastMatrixHash.store(0u,
                                            std::memory_order_relaxed);
    g_runtimeMatrixWriteCount.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixWritePublishCount.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteMissCount.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteBatchCapturedCount.store(0u,
                                                 std::memory_order_relaxed);
    g_runtimeMatrixWriteBatchOverflowCount.store(0u,
                                                 std::memory_order_relaxed);
    g_runtimeMatrixWriteBatchUnreadableCount.store(0u,
                                                   std::memory_order_relaxed);
    g_runtimeMatrixWriteBatchLastGroupCount.store(0u,
                                                  std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperCallCount.store(0u,
                                                std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperPartCount.store(0u,
                                                std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperBindingCount.store(0u,
                                                   std::memory_order_relaxed);
    g_runtimeSimpleGroupPaletteCallCount.store(0u,
                                               std::memory_order_relaxed);
    g_runtimeSimpleGroupPaletteSlotCapturedCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeSimpleGroupPaletteSlotUnreadableCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteBindingQueryHitCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteBindingQueryMissCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteSnapshotCapturedCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteSnapshotTooLargeCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteSnapshotUnreadableCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteSnapshotQueryHitCount.store(
        0u, std::memory_order_relaxed);
    g_renderablePartPaletteSnapshotQueryMissCount.store(
        0u, std::memory_order_relaxed);
    // Phase 7.47 dt gate probe
    g_spriteUberPreRenderTotalCount.store(0u, std::memory_order_relaxed);
    g_spriteUberPreRenderDtZeroCount.store(0u, std::memory_order_relaxed);
    g_spriteUberPreRenderDtBelowEpsilonCount.store(
        0u, std::memory_order_relaxed);
    g_spriteUberPreRenderDtPositiveCount.store(0u,
                                                std::memory_order_relaxed);
    g_spriteUberPreRenderDtNegativeCount.store(0u,
                                                std::memory_order_relaxed);
    g_spriteUberPreRenderLastDtBits.store(0u, std::memory_order_relaxed);
    g_spriteUberPreRenderLastZeroDtFrameTag.store(
        0u, std::memory_order_relaxed);
    g_spriteUberPreRenderLastPositiveDtFrameTag.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteLastFrameTag.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteFramesWithHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteFramesEmptyCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperLastFrameTag.store(
        0u, std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperFramesWithHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeGroupPaletteWrapperFramesEmptyCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeSimpleGroupPaletteLastFrameTag.store(
        0u, std::memory_order_relaxed);
    g_runtimeSimpleGroupPaletteFramesWithHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeSimpleGroupPaletteFramesEmptyCount.store(
        0u, std::memory_order_relaxed);
    g_paletteFrameTagLastSeen.store(0u, std::memory_order_relaxed);
    g_paletteFrameTagAdvanceCount.store(0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteLastRuntimeModelPtr.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixWriteLastMatrixIndex.store(0u,
                                              std::memory_order_relaxed);
    g_runtimeMatrixWriteLastMatrixCount.store(0u,
                                              std::memory_order_relaxed);
    g_runtimeMatrixWriteLastMatrixHash.store(0u,
                                             std::memory_order_relaxed);
    {
      std::unique_lock<std::shared_mutex> lock(g_runtimePoseArrayRangeMutex);
      g_runtimePoseArrayByModel.clear();
      g_runtimePoseArrayByMatrixPtr.clear();
    }
    render::ResetCurrentDrawContractCache();
    g_runtimeMatrixRangeCopyPalettePublishHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyPalettePublishMissCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyPaletteFallbackCModelCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixFlushPaletteSuppressedCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyLastRuntimeModelPtr.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyLastContextPtr.store(0u,
                                                 std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyLastSourceBasePtr.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyLastMatrixCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixRangeCopyLastMatrixHash.store(0u,
                                                 std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentRootHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentOwnerHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentChildHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentAliasHitCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentRootPaletteReadyCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount.store(
        0u, std::memory_order_relaxed);
    g_runtimeMatrixPublisherAttachmentChildPaletteReadyCount.store(
        0u, std::memory_order_relaxed);
    g_attachmentChildLineageBootstrapAttemptCount.store(
        0u, std::memory_order_relaxed);
    g_attachmentChildLineageBootstrapSuccessCount.store(
        0u, std::memory_order_relaxed);
    g_attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount.store(
        0u, std::memory_order_relaxed);
    g_attachmentChildLineageBootstrapMissNoModelDataLinksCount.store(
        0u, std::memory_order_relaxed);
    g_attachmentChildLineageBootstrapMissNoUniqueChildCount.store(
        0u, std::memory_order_relaxed);
    g_attachmentAncestorIdentityHintWriteCount.store(
        0u, std::memory_order_relaxed);
    g_sourceObjectRenderBridgeResolvedByEntryCount.store(
        0u, std::memory_order_relaxed);
    g_sourceObjectRenderBridgeResolvedBySceneNodeCount.store(
        0u, std::memory_order_relaxed);
  g_spriteHostBindResolvedIdentityCount.store(0u,
                                              std::memory_order_relaxed);
  g_spriteHostBindResolvedUnitCount.store(0u, std::memory_order_relaxed);
  g_spriteHostBindResolvedHandleCount.store(0u, std::memory_order_relaxed);
  g_spriteHostBindResolvedRawcodeCount.store(0u, std::memory_order_relaxed);
  g_spriteFrameSourceHintCount.store(0u, std::memory_order_relaxed);
  g_spriteFrameSourceResolvedIdentityCount.store(0u,
                                                 std::memory_order_relaxed);
  g_spriteFrameSourceResolvedUnitCount.store(0u, std::memory_order_relaxed);
  g_spriteFrameSourceResolvedHandleCount.store(0u, std::memory_order_relaxed);
  g_spriteFrameSourceResolvedRawcodeCount.store(0u, std::memory_order_relaxed);
  g_spriteFrameSourceBaseAliasPublishCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameSourceDeepIdentityResolvedCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameSourceObjectRuntimeFieldCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameSourceObjectRegistryFieldHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFramePoseBaseAliasPublishCount.store(0u,
                                               std::memory_order_relaxed);
  g_spriteFramePoseBaseAliasMatrixPaletteCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentRootRuntimeHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentOwnerRuntimeHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentChildRuntimeHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentContextHintCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentFullUpdateHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentLiteUpdateHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentCallerKnownCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentCallerChangedCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentAttachScopeHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentAttachScopeOwnerHitCount.store(
      0u, std::memory_order_relaxed);
  g_spriteFrameAttachmentAttachScopeParentRuntimeMatchCount.store(
      0u, std::memory_order_relaxed);
  g_attachedEffectInitBindCount.store(0u, std::memory_order_relaxed);
  g_attachedEffectInitResolvedIdentityCount.store(
      0u, std::memory_order_relaxed);
  g_attachedEffectInitResolvedUnitCount.store(0u,
                                              std::memory_order_relaxed);
  g_attachedEffectInitResolvedHandleCount.store(0u,
                                                std::memory_order_relaxed);
  g_attachedEffectInitResolvedRawcodeCount.store(0u,
                                                 std::memory_order_relaxed);
  g_attachedEffectInitParentRuntimeOwnerPublishCount.store(
      0u, std::memory_order_relaxed);
  g_attachedEffectDirectBindCount.store(0u, std::memory_order_relaxed);
  g_attachedEffectDirectResolvedIdentityCount.store(
      0u, std::memory_order_relaxed);
  g_attachedEffectDirectResolvedUnitCount.store(0u,
                                                std::memory_order_relaxed);
  g_attachedEffectDirectResolvedHandleCount.store(0u,
                                                  std::memory_order_relaxed);
  g_attachedEffectDirectResolvedRawcodeCount.store(0u,
                                                   std::memory_order_relaxed);
  g_attachModelToPointBindCount.store(0u, std::memory_order_relaxed);
  g_attachModelToPointResolvedIdentityCount.store(0u,
                                                  std::memory_order_relaxed);
  g_attachModelToPointResolvedUnitCount.store(0u, std::memory_order_relaxed);
  g_attachModelToPointResolvedHandleCount.store(0u,
                                                std::memory_order_relaxed);
  g_attachModelToPointResolvedRawcodeCount.store(0u,
                                                 std::memory_order_relaxed);
  g_attachModelToPointPromotedAttachmentChildRuntimeCount.store(
      0u, std::memory_order_relaxed);
  g_attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount.store(
      0u, std::memory_order_relaxed);
  g_currentRenderIdentityHintCount.store(0u, std::memory_order_relaxed);
  g_currentRenderIdentityResolvedCount.store(0u, std::memory_order_relaxed);
  g_sourceObjectIdentityHintResolvedCount.store(0u,
                                                std::memory_order_relaxed);
  g_runtimeSourceObjectPublishCount.store(0u, std::memory_order_relaxed);
  g_attachmentRigidPublishedWithSourceObjectCount.store(
      0u, std::memory_order_relaxed);
  g_attachmentRigidSourceObjectFromChildRuntimeCount.store(
      0u, std::memory_order_relaxed);
  g_attachmentRigidSourceObjectFromOwnerRuntimeCount.store(
      0u, std::memory_order_relaxed);
  g_attachmentRigidSourceObjectFromRootRuntimeCount.store(
      0u, std::memory_order_relaxed);
  g_overrideOutputSampleFrame.store(0u, std::memory_order_relaxed);
  g_overrideOutputLastActiveFrame.store(0u, std::memory_order_relaxed);
  g_overridePrimaryPresetWriteCount.store(0u, std::memory_order_relaxed);
  g_overrideSharedPresetWriteCount.store(0u, std::memory_order_relaxed);
  g_overrideLocalPointWriteCount.store(0u, std::memory_order_relaxed);
  g_overrideLocalPointNonZeroWriteCount.store(0u, std::memory_order_relaxed);
  g_overrideLocalPointObservedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointMatchedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointMatchedChildPaletteReadyWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointMatchedChildLinkBySourceRecordWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointContextRuntimeWithChildLinksWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointContextMatchedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount
      .store(0u, std::memory_order_relaxed);
  g_overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointScratchRootMatchedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount
      .store(0u, std::memory_order_relaxed);
  g_overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArgBlockMatchedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArgBlockIdentityHintWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArg4BlockMatchedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointArg4BlockIdentityHintWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointChildSourceMetaIdentityHintWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointSpriteBoundCandidateWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointParentSpriteIdentityHintWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointRootRuntimeHitWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointRootRuntimeWithChildLinksWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointRootRuntimeMatchedChildLinkWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount
      .store(0u, std::memory_order_relaxed);
  g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount
      .store(0u, std::memory_order_relaxed);
  g_attachmentRigidPublishedCount.store(0u, std::memory_order_relaxed);
  g_overrideMaxPrimaryPresetSlotIndex.store(0u, std::memory_order_relaxed);
  g_overrideMaxSharedPresetSlotIndex.store(0u, std::memory_order_relaxed);
  g_overrideMaxLocalPointSlotIndex.store(0u, std::memory_order_relaxed);
  g_overrideMaxObservedChildLinkCount.store(0u, std::memory_order_relaxed);
  g_overrideMaxObservedChildLinkTag.store(0u, std::memory_order_relaxed);
  g_overrideLastPrimaryPresetHash.store(0u, std::memory_order_relaxed);
  g_overrideLastSharedPresetHash.store(0u, std::memory_order_relaxed);
  g_overrideLastRuntimeModelPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastMatchedChildRuntimeModelPtr.store(0u,
                                                  std::memory_order_relaxed);
  g_overrideLastMatchedChildBySourceRecordRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastContextRuntimeWithChildLinksPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastScratchRootPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastScratchRootRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastArgBlockPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastArgBlockRuntimeModelPtr.store(0u,
                                              std::memory_order_relaxed);
  g_overrideLastArgBlockIdentityHintPtr.store(0u,
                                              std::memory_order_relaxed);
  g_overrideLastArg4BlockPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastArg4BlockRuntimeModelPtr.store(0u,
                                               std::memory_order_relaxed);
  g_overrideLastArg4BlockIdentityHintPtr.store(0u,
                                               std::memory_order_relaxed);
  g_overrideLastChildSourceMetaPtr.store(0u, std::memory_order_relaxed);
  g_overrideLastChildSourceMetaRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastSpriteBoundCandidateSpritePtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastSpriteBoundCandidateRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastParentSpriteIdentityHintSpritePtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastParentSpriteIdentityHintRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_overrideLastRootRuntimeModelPtr.store(0u, std::memory_order_relaxed);
  g_lastSourceObjectRenderBridgeSourceObjectPtr.store(
      0u, std::memory_order_relaxed);
  g_lastSourceObjectRenderBridgeSceneNodePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSourceObjectIdentityHintSourceObjectPtr.store(
      0u, std::memory_order_relaxed);
  g_lastSourceObjectIdentityHintCandidatePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteHostSourceObjectPtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteHostSpritePtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteHostRuntimeModelPtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteHostUnitPtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectPtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceRuntimeModelPtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceBaseRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectVtablePtr.store(0u,
                                               std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectSceneNodeCandidatePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectSpriteCandidatePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectRegistryFieldCandidatePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceDeepIdentityCandidatePtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceWorldObjectEntryPtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceSceneNodePtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceUnitPtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteFramePoseBaseRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFramePoseBaseMatrixCount.store(0u,
                                             std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentSpritePtr.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentRuntimeModelPtr.store(0u,
                                                   std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentContextPtr.store(0u,
                                              std::memory_order_relaxed);
  g_lastAttachedEffectInitOwnerWidgetPtr.store(0u,
                                               std::memory_order_relaxed);
  g_lastAttachedEffectInitChildSpritePtr.store(0u,
                                               std::memory_order_relaxed);
  g_lastAttachedEffectInitChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachedEffectInitParentRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachedEffectInitUnitPtr.store(0u, std::memory_order_relaxed);
  g_lastAttachedEffectDirectOwnerWidgetPtr.store(0u,
                                                 std::memory_order_relaxed);
  g_lastAttachedEffectDirectChildSpritePtr.store(0u,
                                                 std::memory_order_relaxed);
  g_lastAttachedEffectDirectChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachedEffectDirectUnitPtr.store(0u, std::memory_order_relaxed);
  g_lastAttachModelToPointParentSpritePtr.store(0u,
                                                std::memory_order_relaxed);
  g_lastAttachModelToPointChildSpritePtr.store(0u,
                                               std::memory_order_relaxed);
  g_lastAttachModelToPointChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachModelToPointPromotedOwnerRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachModelToPointPromotedChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachModelToPointPromotedChildModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachModelToPointUnitPtr.store(0u, std::memory_order_relaxed);
  g_lastAttachScopeParentSpritePtr.store(0u, std::memory_order_relaxed);
  g_lastAttachScopeParentRuntimeModelPtr.store(0u,
                                               std::memory_order_relaxed);
  g_lastAttachScopeChildSpritePtr.store(0u, std::memory_order_relaxed);
  g_lastAttachScopeChildRuntimeModelPtr.store(0u,
                                              std::memory_order_relaxed);
  g_lastAttachScopeHitRuntimeModelPtr.store(0u,
                                            std::memory_order_relaxed);
  g_lastCurrentRenderIdentityWorldObjectEntryPtr.store(
      0u, std::memory_order_relaxed);
  g_lastCurrentRenderIdentitySceneNodePtr.store(0u, std::memory_order_relaxed);
  g_lastCurrentRenderIdentityUnitPtr.store(0u, std::memory_order_relaxed);
  g_lastRuntimeSourceObjectPtr.store(0u, std::memory_order_relaxed);
  g_lastRuntimeSourceSpriteObjectPtr.store(0u, std::memory_order_relaxed);
  g_lastRuntimeSourceRuntimeModelPtr.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelResolveRuntimeModelPtr.store(0u,
                                                 std::memory_order_relaxed);
  g_lastRuntimeModelResolveHandlePtr.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelCreateRuntimeModelPtr.store(0u,
                                                std::memory_order_relaxed);
  g_lastRuntimeModelCreateModelDataPtr.store(0u,
                                             std::memory_order_relaxed);
  g_lastRuntimeModelInitRuntimeModelPtr.store(0u,
                                              std::memory_order_relaxed);
  g_lastRuntimeModelInitModelDataPtr.store(0u,
                                           std::memory_order_relaxed);
  g_lastAttachmentRigidSourceObjectPtr.store(0u, std::memory_order_relaxed);
  g_lastAttachmentRigidSourceSpriteObjectPtr.store(0u,
                                                   std::memory_order_relaxed);
  g_lastRuntimeChildLinkBuildParentRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildLinkBuildChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildLinkBuildModelDataPtr.store(0u,
                                                std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectParentModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildTimeDirectModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataParentRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataPtr.store(0u,
                                            std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataGroupRecordsPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataHeadPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataLinkNodePtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataChildModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataChildModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherMatchedRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherMatrixCount.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentAncestorFromRuntimeModelPtr.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentAncestorRuntimeModelPtr.store(0u,
                                                std::memory_order_relaxed);
  g_overrideLastLocalPointSlotIndex.store(0u, std::memory_order_relaxed);
  g_overrideLastLocalPointSourceRecordIndex.store(0u,
                                                  std::memory_order_relaxed);
  g_overrideLastObservedChildLinkCount.store(0u, std::memory_order_relaxed);
  g_overrideLastMatchedChildLinkCount.store(0u, std::memory_order_relaxed);
  g_overrideLastMatchedChildMatrixCount.store(0u, std::memory_order_relaxed);
  g_overrideLastMatchedChildBySourceRecordLinkCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLastMatchedChildBySourceRecordMatrixCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLastContextRuntimeWithChildLinksOffset.store(
      0u, std::memory_order_relaxed);
  g_overrideLastContextRuntimeWithChildLinksCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLastContextRuntimeWithChildLinksMaxTag.store(
      0u, std::memory_order_relaxed);
  g_overrideLastScratchRootRuntimeChildLinkCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLastScratchRootRuntimeMaxTag.store(
      0u, std::memory_order_relaxed);
  g_overrideLastArgBlockRuntimeOffset.store(0u, std::memory_order_relaxed);
  g_overrideLastArgBlockRuntimeChildLinkCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLastArgBlockRuntimeMaxTag.store(0u, std::memory_order_relaxed);
  g_overrideLastArgBlockIdentityHintOffset.store(0u,
                                                 std::memory_order_relaxed);
  g_overrideLastArg4BlockRuntimeOffset.store(0u, std::memory_order_relaxed);
  g_overrideLastArg4BlockRuntimeChildLinkCount.store(
      0u, std::memory_order_relaxed);
  g_overrideLastArg4BlockRuntimeMaxTag.store(0u, std::memory_order_relaxed);
  g_overrideLastArg4BlockIdentityHintOffset.store(0u,
                                                  std::memory_order_relaxed);
  g_overrideLastRootRuntimeChildLinkCount.store(0u,
                                                std::memory_order_relaxed);
  g_overrideLastRootRuntimeMaxTag.store(0u, std::memory_order_relaxed);
  g_lastSpriteHostJHandle.store(0u, std::memory_order_relaxed);
  g_lastSpriteHostRawcode.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceJHandle.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceRawcode.store(0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectRuntimeFieldOffset.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceObjectRegistryFieldOffset.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameSourceDeepIdentityOffset.store(
      0u, std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentRoleMask.store(0u,
                                            std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentUpdateKind.store(0u,
                                              std::memory_order_relaxed);
  g_lastSpriteFrameAttachmentCallerRva.store(0u,
                                             std::memory_order_relaxed);
  g_lastSourceObjectIdentityHintOffset.store(0u,
                                             std::memory_order_relaxed);
  g_lastAttachedEffectInitJHandle.store(0u, std::memory_order_relaxed);
  g_lastAttachedEffectInitRawcode.store(0u, std::memory_order_relaxed);
  g_lastAttachedEffectDirectJHandle.store(0u, std::memory_order_relaxed);
  g_lastAttachedEffectDirectRawcode.store(0u, std::memory_order_relaxed);
  g_lastAttachModelToPointJHandle.store(0u, std::memory_order_relaxed);
  g_lastAttachModelToPointRawcode.store(0u, std::memory_order_relaxed);
  g_lastAttachModelToPointAttachPointIndex.store(
      0u, std::memory_order_relaxed);
  g_lastAttachScopeCallerRva.store(0u, std::memory_order_relaxed);
  g_lastAttachScopeHitRoleMask.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelCtorRuntimeModelPtr.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelCtorCallerRva.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelCtorKind.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelResolveCallerRva.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelCreateCallerRva.store(0u, std::memory_order_relaxed);
  g_lastRuntimeModelInitCallerRva.store(0u, std::memory_order_relaxed);
  g_lastRuntimeChildLinkBuildSourceMeta.store(0u,
                                              std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataPhase.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataGroupCount.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataLinkCount.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataUnreadableLinkCount.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeChildBuildModelDataSourceMeta.store(
      0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherKind.store(0u, std::memory_order_relaxed);
  g_lastRuntimeMatrixPublisherRoleMask.store(0u,
                                             std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapModelDataLinkCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapRuntimeLinkCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapStrictCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapSourceCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapBucketCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapAllCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentChildLineageBootstrapModelDataBucketCount.store(
      0u, std::memory_order_relaxed);
  g_lastAttachmentAncestorDepth.store(0u, std::memory_order_relaxed);
  g_overrideLastLocalPointXBits.store(0u, std::memory_order_relaxed);
  g_overrideLastLocalPointYBits.store(0u, std::memory_order_relaxed);
  g_overrideLastLocalPointZBits.store(0u, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(g_runtimeParentLinkMutex);
    g_runtimeParentLinks.clear();
  }
  {
    std::lock_guard<std::mutex> lock(g_runtimePaletteTreeDedupeMutex);
    g_runtimePaletteTreeDedupeFrame = 0u;
    g_runtimePaletteTreeDedupeRoots.clear();
    g_runtimePaletteTreeDedupeOwnerRoots.clear();
  }
  }

  bool installed = false;
  if (!g_bootstrapHooksInstalled.load(std::memory_order_relaxed)) {
    if constexpr (dxvk::war3::internal::
                      kShadowRuntimeModelBootstrapCtorHooksEnabled) {
      installed = InstallRuntimeModelPlainCtorHook(gameBase) || installed;
      installed = InstallRuntimeModelComplexCtorHook(gameBase) || installed;
    }
    if constexpr (dxvk::war3::internal::
                      kShadowRuntimeModelBootstrapResolveHookEnabled) {
      installed = InstallResolveRuntimeModelFromHandleHook(gameBase) || installed;
    }
    if constexpr (dxvk::war3::internal::
                      kShadowRuntimeModelBootstrapPromoteHookEnabled) {
      installed = InstallPromoteRuntimeModelHook(gameBase) || installed;
    }
    if constexpr (dxvk::war3::internal::
                      kShadowRuntimeModelBootstrapInitCopyHookEnabled) {
      installed = InstallRuntimeInitFromModelDataHook(gameBase) || installed;
    }
    if constexpr (dxvk::war3::internal::
                      kShadowRuntimeModelBootstrapChildLinkHookEnabled) {
      if (g_config.attachmentEnabled)
        installed = InstallBuildChildRuntimeModelLinksHook(gameBase) || installed;
    }
    if (installed)
      g_bootstrapHooksInstalled.store(true, std::memory_order_relaxed);
  }

  if (!bootstrapOnly && !g_fullHooksInstalled.load(std::memory_order_relaxed)) {
    bool fullInstalled = false;
    fullInstalled = InstallCreateSpriteAndBindSourceObjectHook(gameBase) ||
                    fullInstalled;
    if (g_config.attachmentEnabled) {
      fullInstalled = InstallAttachedEffectInitHook(gameBase) || fullInstalled;
      fullInstalled =
          InstallAttachedEffectDirectAttachHook(gameBase) || fullInstalled;
      fullInstalled = InstallAttachModelToPointHook(gameBase) || fullInstalled;
    }
    fullInstalled = InstallCreateSpriteRuntimeHook(gameBase) || fullInstalled;
    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDisableSemanticGeosetResourceCapture) {
      fullInstalled =
          InstallCreateGeosetFromRawArraysHook(gameBase) || fullInstalled;
    }
    const bool installSpriteFrameHooks =
        g_config.poseEnabled ||
        dxvk::war3::internal::
            kWar3RuntimeConfigInstallSpriteFrameHooksWithoutPose ||
        SpriteUberDtProbeEnabled();
    if (installSpriteFrameHooks) {
      fullInstalled = InstallSpriteMiniFrameUpdateHook(gameBase) || fullInstalled;
      fullInstalled =
          InstallSpriteMiniFrameLiteUpdateHook(gameBase) || fullInstalled;
      fullInstalled = InstallSpriteFrameUpdateHook(gameBase) || fullInstalled;
      fullInstalled =
          InstallSpriteFrameLiteUpdateHook(gameBase) || fullInstalled;
    }
    if (g_config.poseEnabled || runtimePoseUpdateEnabled)
      fullInstalled = InstallRuntimePoseHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled || runtimeMatrixWriteEnabled)
      fullInstalled = InstallRuntimeMatrixWriteHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled || runtimeMatrixWriteEnabled) {
      fullInstalled =
          InstallRuntimeGroupPaletteWrapperHook(gameBase) || fullInstalled;
      fullInstalled =
          InstallRuntimeSimpleGroupPaletteHook(gameBase) || fullInstalled;
    }
    if (g_config.enabled)
      fullInstalled =
          render::InstallCurrentDrawContractHook(gameBase,
                                                 g_config.logEnabled) ||
          fullInstalled;
    if (g_config.poseEnabled)
      fullInstalled =
          InstallRuntimePropagatePoseTreeHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled)
      fullInstalled =
          InstallRuntimeRecurseChildTreeHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled || matrixPublisherPoseEnabled)
      fullInstalled =
          InstallRuntimeMatrixRangeCopyHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled || matrixPublisherPoseEnabled)
      fullInstalled = InstallRuntimeMatrixFlushHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled && g_config.attachmentEnabled)
      fullInstalled =
          InstallRuntimeWriteSharedPresetOutputHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled && g_config.attachmentEnabled)
      fullInstalled =
          InstallRuntimeWriteLocalPointOutputHook(gameBase) || fullInstalled;
    if (g_config.poseEnabled && g_config.attachmentEnabled)
      fullInstalled =
          InstallRuntimeWritePrimaryPresetOutputHook(gameBase) || fullInstalled;
    installed = fullInstalled || installed;
    if (fullInstalled)
      g_fullHooksInstalled.store(true, std::memory_order_relaxed);
  }

  g_active.store(g_bootstrapHooksInstalled.load(std::memory_order_relaxed) ||
                     g_fullHooksInstalled.load(std::memory_order_relaxed),
                 std::memory_order_relaxed);
}

void Shutdown() {
  g_active.store(false, std::memory_order_relaxed);
  g_bootstrapHooksInstalled.store(false, std::memory_order_relaxed);
  g_fullHooksInstalled.store(false, std::memory_order_relaxed);
  g_gameBase = 0u;
}

bool IsActive() { return g_active.load(std::memory_order_relaxed); }

bool IsPoseHookEnabled() { return g_config.poseEnabled; }

uint64_t RuntimeMatrixPublisherPoseRevision() {
  return g_runtimeMatrixPublisherPoseRevision.load(std::memory_order_relaxed);
}

RuntimeOverrideOutputProbeSummary QueryRuntimeOverrideOutputProbeSummary() {
  RuntimeOverrideOutputProbeSummary summary = {};
  summary.runtimeModelCtorCount =
      g_runtimeModelCtorCount.load(std::memory_order_relaxed);
  summary.runtimeModelComplexCtorCount =
      g_runtimeModelComplexCtorCount.load(std::memory_order_relaxed);
  summary.runtimeModelPlainCtorCount =
      g_runtimeModelPlainCtorCount.load(std::memory_order_relaxed);
  summary.runtimeModelCtorCallerPromoteCount =
      g_runtimeModelCtorCallerPromoteCount.load(std::memory_order_relaxed);
  summary.runtimeModelCtorCallerOtherCount =
      g_runtimeModelCtorCallerOtherCount.load(std::memory_order_relaxed);
  summary.runtimeModelCreateCount =
      g_runtimeModelCreateCount.load(std::memory_order_relaxed);
  summary.runtimeModelResolveCount =
      g_runtimeModelResolveCount.load(std::memory_order_relaxed);
  summary.runtimeModelResolveResolvedIdentityCount =
      g_runtimeModelResolveResolvedIdentityCount.load(
          std::memory_order_relaxed);
  summary.runtimeModelCreateCallerBuildChildLinksCount =
      g_runtimeModelCreateCallerBuildChildLinksCount.load(
          std::memory_order_relaxed);
  summary.runtimeModelCreateCallerCreateSpriteRuntimeCount =
      g_runtimeModelCreateCallerCreateSpriteRuntimeCount.load(
          std::memory_order_relaxed);
  summary.runtimeModelCreateCallerOtherCount =
      g_runtimeModelCreateCallerOtherCount.load(std::memory_order_relaxed);
  summary.runtimeModelInitCopyCount =
      g_runtimeModelInitCopyCount.load(std::memory_order_relaxed);
  summary.runtimeModelInitCopyPublishedFallbackCount =
      g_runtimeModelInitCopyPublishedFallbackCount.load(
          std::memory_order_relaxed);
  summary.runtimeChildLinkBuildCount =
      g_runtimeChildLinkBuildCount.load(std::memory_order_relaxed);
  summary.runtimeChildLinkBuiltChildCount =
      g_runtimeChildLinkBuiltChildCount.load(std::memory_order_relaxed);
  summary.runtimeChildBuildTimeDirectPublishCount =
      g_runtimeChildBuildTimeDirectPublishCount.load(
          std::memory_order_relaxed);
  summary.runtimeChildBuildTimeDirectPublishWithResourceCount =
      g_runtimeChildBuildTimeDirectPublishWithResourceCount.load(
          std::memory_order_relaxed);
  summary.runtimeChildBuildModelDataPreLinkCount =
      g_runtimeChildBuildModelDataPreLinkCount.load(
          std::memory_order_relaxed);
  summary.runtimeChildBuildModelDataPostLinkCount =
      g_runtimeChildBuildModelDataPostLinkCount.load(
          std::memory_order_relaxed);
  summary.runtimeChildBuildModelDataPreUnreadableLinkCount =
      g_runtimeChildBuildModelDataPreUnreadableLinkCount.load(
          std::memory_order_relaxed);
  summary.runtimeChildBuildModelDataPostUnreadableLinkCount =
      g_runtimeChildBuildModelDataPostUnreadableLinkCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyCount =
      g_runtimeMatrixRangeCopyCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixFlushCount =
      g_runtimeMatrixFlushCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixPublisherPaletteReadyCount =
      g_runtimeMatrixPublisherPaletteReadyCount.load(
          std::memory_order_relaxed);
  summary.runtimePoseUpdatePalettePublishCount =
      g_runtimePoseUpdatePalettePublishCount.load(std::memory_order_relaxed);
  summary.runtimePoseUpdateLastRuntimeModelPtr =
      g_runtimePoseUpdateLastRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.runtimePoseUpdateLastMatrixCount =
      g_runtimePoseUpdateLastMatrixCount.load(std::memory_order_relaxed);
  summary.runtimePoseUpdateLastMatrixHash =
      g_runtimePoseUpdateLastMatrixHash.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteCount =
      g_runtimeMatrixWriteCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixWritePublishCount =
      g_runtimeMatrixWritePublishCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteMissCount =
      g_runtimeMatrixWriteMissCount.load(std::memory_order_relaxed);
  // Phase 7.31 P0 batch-capture 指标直接并入模型 hook summary。
  summary.runtimeMatrixWriteBatchCapturedCount =
      g_runtimeMatrixWriteBatchCapturedCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteBatchOverflowCount =
      g_runtimeMatrixWriteBatchOverflowCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteBatchUnreadableCount =
      g_runtimeMatrixWriteBatchUnreadableCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixWriteBatchLastGroupCount =
      g_runtimeMatrixWriteBatchLastGroupCount.load(std::memory_order_relaxed);
  summary.runtimeGroupPaletteWrapperCallCount =
      g_runtimeGroupPaletteWrapperCallCount.load(std::memory_order_relaxed);
  summary.runtimeGroupPaletteWrapperPartCount =
      g_runtimeGroupPaletteWrapperPartCount.load(std::memory_order_relaxed);
  summary.runtimeGroupPaletteWrapperBindingCount =
      g_runtimeGroupPaletteWrapperBindingCount.load(std::memory_order_relaxed);
  summary.runtimeSimpleGroupPaletteCallCount =
      g_runtimeSimpleGroupPaletteCallCount.load(std::memory_order_relaxed);
  summary.runtimeSimpleGroupPaletteSlotCapturedCount =
      g_runtimeSimpleGroupPaletteSlotCapturedCount.load(
          std::memory_order_relaxed);
  summary.runtimeSimpleGroupPaletteSlotUnreadableCount =
      g_runtimeSimpleGroupPaletteSlotUnreadableCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteBindingQueryHitCount =
      g_renderablePartPaletteBindingQueryHitCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteBindingQueryMissCount =
      g_renderablePartPaletteBindingQueryMissCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteSnapshotCapturedCount =
      g_renderablePartPaletteSnapshotCapturedCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteSnapshotTooLargeCount =
      g_renderablePartPaletteSnapshotTooLargeCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteSnapshotUnreadableCount =
      g_renderablePartPaletteSnapshotUnreadableCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteSnapshotQueryHitCount =
      g_renderablePartPaletteSnapshotQueryHitCount.load(
          std::memory_order_relaxed);
  summary.renderablePartPaletteSnapshotQueryMissCount =
      g_renderablePartPaletteSnapshotQueryMissCount.load(
          std::memory_order_relaxed);
  // Phase 7.47 dt gate probe
  summary.spriteUberPreRenderTotalCount =
      g_spriteUberPreRenderTotalCount.load(std::memory_order_relaxed);
  summary.spriteUberPreRenderDtZeroCount =
      g_spriteUberPreRenderDtZeroCount.load(std::memory_order_relaxed);
  summary.spriteUberPreRenderDtBelowEpsilonCount =
      g_spriteUberPreRenderDtBelowEpsilonCount.load(
          std::memory_order_relaxed);
  summary.spriteUberPreRenderDtPositiveCount =
      g_spriteUberPreRenderDtPositiveCount.load(std::memory_order_relaxed);
  summary.spriteUberPreRenderDtNegativeCount =
      g_spriteUberPreRenderDtNegativeCount.load(std::memory_order_relaxed);
  summary.spriteUberPreRenderLastDtBits =
      uint64_t(g_spriteUberPreRenderLastDtBits.load(
          std::memory_order_relaxed));
  summary.spriteUberPreRenderLastZeroDtFrameTag =
      uint64_t(g_spriteUberPreRenderLastZeroDtFrameTag.load(
          std::memory_order_relaxed));
  summary.spriteUberPreRenderLastPositiveDtFrameTag =
      uint64_t(g_spriteUberPreRenderLastPositiveDtFrameTag.load(
          std::memory_order_relaxed));
  summary.runtimeMatrixWriteFramesWithHitCount =
      g_runtimeMatrixWriteFramesWithHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixWriteFramesEmptyCount =
      g_runtimeMatrixWriteFramesEmptyCount.load(std::memory_order_relaxed);
  summary.runtimeGroupPaletteWrapperFramesWithHitCount =
      g_runtimeGroupPaletteWrapperFramesWithHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeGroupPaletteWrapperFramesEmptyCount =
      g_runtimeGroupPaletteWrapperFramesEmptyCount.load(
          std::memory_order_relaxed);
  summary.runtimeSimpleGroupPaletteFramesWithHitCount =
      g_runtimeSimpleGroupPaletteFramesWithHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeSimpleGroupPaletteFramesEmptyCount =
      g_runtimeSimpleGroupPaletteFramesEmptyCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixWriteLastRuntimeModelPtr =
      g_runtimeMatrixWriteLastRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteLastMatrixIndex =
      g_runtimeMatrixWriteLastMatrixIndex.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteLastMatrixCount =
      g_runtimeMatrixWriteLastMatrixCount.load(std::memory_order_relaxed);
  summary.runtimeMatrixWriteLastMatrixHash =
      g_runtimeMatrixWriteLastMatrixHash.load(std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyPalettePublishHitCount =
      g_runtimeMatrixRangeCopyPalettePublishHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyPalettePublishMissCount =
      g_runtimeMatrixRangeCopyPalettePublishMissCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyPaletteFallbackCModelCount =
      g_runtimeMatrixRangeCopyPaletteFallbackCModelCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixFlushPaletteSuppressedCount =
      g_runtimeMatrixFlushPaletteSuppressedCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyLastRuntimeModelPtr =
      g_runtimeMatrixRangeCopyLastRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyLastContextPtr =
      g_runtimeMatrixRangeCopyLastContextPtr.load(std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyLastSourceBasePtr =
      g_runtimeMatrixRangeCopyLastSourceBasePtr.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyLastMatrixCount =
      g_runtimeMatrixRangeCopyLastMatrixCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixRangeCopyLastMatrixHash =
      g_runtimeMatrixRangeCopyLastMatrixHash.load(std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentRootHitCount =
      g_runtimeMatrixPublisherAttachmentRootHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentOwnerHitCount =
      g_runtimeMatrixPublisherAttachmentOwnerHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentChildHitCount =
      g_runtimeMatrixPublisherAttachmentChildHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentAliasHitCount =
      g_runtimeMatrixPublisherAttachmentAliasHitCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentRootPaletteReadyCount =
      g_runtimeMatrixPublisherAttachmentRootPaletteReadyCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount =
      g_runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount.load(
          std::memory_order_relaxed);
  summary.runtimeMatrixPublisherAttachmentChildPaletteReadyCount =
      g_runtimeMatrixPublisherAttachmentChildPaletteReadyCount.load(
          std::memory_order_relaxed);
  summary.attachmentChildLineageBootstrapAttemptCount =
      g_attachmentChildLineageBootstrapAttemptCount.load(
          std::memory_order_relaxed);
  summary.attachmentChildLineageBootstrapSuccessCount =
      g_attachmentChildLineageBootstrapSuccessCount.load(
          std::memory_order_relaxed);
  summary.attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount =
      g_attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount.load(
          std::memory_order_relaxed);
  summary.attachmentChildLineageBootstrapMissNoModelDataLinksCount =
      g_attachmentChildLineageBootstrapMissNoModelDataLinksCount.load(
          std::memory_order_relaxed);
  summary.attachmentChildLineageBootstrapMissNoUniqueChildCount =
      g_attachmentChildLineageBootstrapMissNoUniqueChildCount.load(
          std::memory_order_relaxed);
  summary.attachmentAncestorIdentityHintWriteCount =
      g_attachmentAncestorIdentityHintWriteCount.load(
          std::memory_order_relaxed);
  summary.sourceObjectRenderBridgeResolvedByEntryCount =
      g_sourceObjectRenderBridgeResolvedByEntryCount.load(
          std::memory_order_relaxed);
  summary.sourceObjectRenderBridgeResolvedBySceneNodeCount =
      g_sourceObjectRenderBridgeResolvedBySceneNodeCount.load(
          std::memory_order_relaxed);
  summary.spriteHostBindCount =
      g_spriteHostBindCount.load(std::memory_order_relaxed);
  summary.spriteHostBindResolvedIdentityCount =
      g_spriteHostBindResolvedIdentityCount.load(std::memory_order_relaxed);
  summary.spriteHostBindResolvedUnitCount =
      g_spriteHostBindResolvedUnitCount.load(std::memory_order_relaxed);
  summary.spriteHostBindResolvedHandleCount =
      g_spriteHostBindResolvedHandleCount.load(std::memory_order_relaxed);
  summary.spriteHostBindResolvedRawcodeCount =
      g_spriteHostBindResolvedRawcodeCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceHintCount =
      g_spriteFrameSourceHintCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceResolvedIdentityCount =
      g_spriteFrameSourceResolvedIdentityCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceResolvedUnitCount =
      g_spriteFrameSourceResolvedUnitCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceResolvedHandleCount =
      g_spriteFrameSourceResolvedHandleCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceResolvedRawcodeCount =
      g_spriteFrameSourceResolvedRawcodeCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceBaseAliasPublishCount =
      g_spriteFrameSourceBaseAliasPublishCount.load(std::memory_order_relaxed);
  summary.spriteFrameSourceDeepIdentityResolvedCount =
      g_spriteFrameSourceDeepIdentityResolvedCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameSourceObjectRuntimeFieldCandidateCount =
      g_spriteFrameSourceObjectRuntimeFieldCandidateCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameSourceObjectRegistryFieldHitCount =
      g_spriteFrameSourceObjectRegistryFieldHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFramePoseBaseAliasPublishCount =
      g_spriteFramePoseBaseAliasPublishCount.load(std::memory_order_relaxed);
  summary.spriteFramePoseBaseAliasMatrixPaletteCount =
      g_spriteFramePoseBaseAliasMatrixPaletteCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentRootRuntimeHitCount =
      g_spriteFrameAttachmentRootRuntimeHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentOwnerRuntimeHitCount =
      g_spriteFrameAttachmentOwnerRuntimeHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentChildRuntimeHitCount =
      g_spriteFrameAttachmentChildRuntimeHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentContextHintCount =
      g_spriteFrameAttachmentContextHintCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentFullUpdateHitCount =
      g_spriteFrameAttachmentFullUpdateHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentLiteUpdateHitCount =
      g_spriteFrameAttachmentLiteUpdateHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentCallerKnownCount =
      g_spriteFrameAttachmentCallerKnownCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentCallerChangedCount =
      g_spriteFrameAttachmentCallerChangedCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentAttachScopeHitCount =
      g_spriteFrameAttachmentAttachScopeHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentAttachScopeOwnerHitCount =
      g_spriteFrameAttachmentAttachScopeOwnerHitCount.load(
          std::memory_order_relaxed);
  summary.spriteFrameAttachmentAttachScopeParentRuntimeMatchCount =
      g_spriteFrameAttachmentAttachScopeParentRuntimeMatchCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectInitBindCount =
      g_attachedEffectInitBindCount.load(std::memory_order_relaxed);
  summary.attachedEffectInitResolvedIdentityCount =
      g_attachedEffectInitResolvedIdentityCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectInitResolvedUnitCount =
      g_attachedEffectInitResolvedUnitCount.load(std::memory_order_relaxed);
  summary.attachedEffectInitResolvedHandleCount =
      g_attachedEffectInitResolvedHandleCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectInitResolvedRawcodeCount =
      g_attachedEffectInitResolvedRawcodeCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectInitParentRuntimeOwnerPublishCount =
      g_attachedEffectInitParentRuntimeOwnerPublishCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectDirectBindCount =
      g_attachedEffectDirectBindCount.load(std::memory_order_relaxed);
  summary.attachedEffectDirectResolvedIdentityCount =
      g_attachedEffectDirectResolvedIdentityCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectDirectResolvedUnitCount =
      g_attachedEffectDirectResolvedUnitCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectDirectResolvedHandleCount =
      g_attachedEffectDirectResolvedHandleCount.load(
          std::memory_order_relaxed);
  summary.attachedEffectDirectResolvedRawcodeCount =
      g_attachedEffectDirectResolvedRawcodeCount.load(
          std::memory_order_relaxed);
  summary.attachModelToPointBindCount =
      g_attachModelToPointBindCount.load(std::memory_order_relaxed);
  summary.attachModelToPointResolvedIdentityCount =
      g_attachModelToPointResolvedIdentityCount.load(
          std::memory_order_relaxed);
  summary.attachModelToPointResolvedUnitCount =
      g_attachModelToPointResolvedUnitCount.load(std::memory_order_relaxed);
  summary.attachModelToPointResolvedHandleCount =
      g_attachModelToPointResolvedHandleCount.load(
          std::memory_order_relaxed);
  summary.attachModelToPointResolvedRawcodeCount =
      g_attachModelToPointResolvedRawcodeCount.load(
          std::memory_order_relaxed);
  summary.attachModelToPointPromotedAttachmentChildRuntimeCount =
      g_attachModelToPointPromotedAttachmentChildRuntimeCount.load(
          std::memory_order_relaxed);
  summary.attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount =
      g_attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount.load(
          std::memory_order_relaxed);
  summary.currentRenderIdentityHintCount =
      g_currentRenderIdentityHintCount.load(std::memory_order_relaxed);
  summary.currentRenderIdentityResolvedCount =
      g_currentRenderIdentityResolvedCount.load(std::memory_order_relaxed);
  summary.sourceObjectIdentityHintResolvedCount =
      g_sourceObjectIdentityHintResolvedCount.load(
          std::memory_order_relaxed);
  summary.runtimeSourceObjectPublishCount =
      g_runtimeSourceObjectPublishCount.load(std::memory_order_relaxed);
  summary.attachmentRigidPublishedWithSourceObjectCount =
      g_attachmentRigidPublishedWithSourceObjectCount.load(
          std::memory_order_relaxed);
  summary.attachmentRigidSourceObjectFromChildRuntimeCount =
      g_attachmentRigidSourceObjectFromChildRuntimeCount.load(
          std::memory_order_relaxed);
  summary.attachmentRigidSourceObjectFromOwnerRuntimeCount =
      g_attachmentRigidSourceObjectFromOwnerRuntimeCount.load(
          std::memory_order_relaxed);
  summary.attachmentRigidSourceObjectFromRootRuntimeCount =
      g_attachmentRigidSourceObjectFromRootRuntimeCount.load(
          std::memory_order_relaxed);
  summary.sampleFrame =
      g_overrideOutputSampleFrame.load(std::memory_order_relaxed);
  summary.lastActiveFrame =
      g_overrideOutputLastActiveFrame.load(std::memory_order_relaxed);
  summary.primaryPresetWriteCount =
      g_overridePrimaryPresetWriteCount.load(std::memory_order_relaxed);
  summary.sharedPresetWriteCount =
      g_overrideSharedPresetWriteCount.load(std::memory_order_relaxed);
  summary.localPointWriteCount =
      g_overrideLocalPointWriteCount.load(std::memory_order_relaxed);
  summary.localPointNonZeroWriteCount =
      g_overrideLocalPointNonZeroWriteCount.load(std::memory_order_relaxed);
  summary.localPointObservedChildLinkWriteCount =
      g_overrideLocalPointObservedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointMatchedChildLinkWriteCount =
      g_overrideLocalPointMatchedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointMatchedChildPaletteReadyWriteCount =
      g_overrideLocalPointMatchedChildPaletteReadyWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointMatchedChildLinkBySourceRecordWriteCount =
      g_overrideLocalPointMatchedChildLinkBySourceRecordWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointMatchedChildPaletteReadyBySourceRecordWriteCount =
      g_overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary.localPointContextRuntimeWithChildLinksWriteCount =
      g_overrideLocalPointContextRuntimeWithChildLinksWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointContextMatchedChildLinkWriteCount =
      g_overrideLocalPointContextMatchedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointContextMatchedChildLinkBySourceRecordWriteCount =
      g_overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointContextMatchedChildPaletteReadyBySourceRecordWriteCount =
      g_overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary.localPointScratchRootRuntimeWithChildLinksWriteCount =
      g_overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointScratchRootMatchedChildLinkWriteCount =
      g_overrideLocalPointScratchRootMatchedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointScratchRootMatchedChildLinkBySourceRecordWriteCount =
      g_overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary
      .localPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount =
      g_overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary.localPointArgBlockRuntimeWithChildLinksWriteCount =
      g_overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointArgBlockMatchedChildLinkWriteCount =
      g_overrideLocalPointArgBlockMatchedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointArgBlockMatchedChildLinkBySourceRecordWriteCount =
      g_overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointArgBlockIdentityHintWriteCount =
      g_overrideLocalPointArgBlockIdentityHintWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointArg4BlockRuntimeWithChildLinksWriteCount =
      g_overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointArg4BlockMatchedChildLinkWriteCount =
      g_overrideLocalPointArg4BlockMatchedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointArg4BlockMatchedChildLinkBySourceRecordWriteCount =
      g_overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary.localPointArg4BlockIdentityHintWriteCount =
      g_overrideLocalPointArg4BlockIdentityHintWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointChildSourceMetaIdentityHintWriteCount =
      g_overrideLocalPointChildSourceMetaIdentityHintWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointSpriteBoundCandidateWriteCount =
      g_overrideLocalPointSpriteBoundCandidateWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointParentSpriteIdentityHintWriteCount =
      g_overrideLocalPointParentSpriteIdentityHintWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointRootRuntimeHitWriteCount =
      g_overrideLocalPointRootRuntimeHitWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointRootRuntimeWithChildLinksWriteCount =
      g_overrideLocalPointRootRuntimeWithChildLinksWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointRootRuntimeMatchedChildLinkWriteCount =
      g_overrideLocalPointRootRuntimeMatchedChildLinkWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointRootRuntimeMatchedChildPaletteReadyWriteCount =
      g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount.load(
          std::memory_order_relaxed);
  summary.localPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount =
      g_overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary
      .localPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount =
      g_overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount
          .load(std::memory_order_relaxed);
  summary.attachmentRigidPublishedCount =
      g_attachmentRigidPublishedCount.load(std::memory_order_relaxed);
  summary.maxPrimaryPresetSlotIndex =
      g_overrideMaxPrimaryPresetSlotIndex.load(std::memory_order_relaxed);
  summary.maxSharedPresetSlotIndex =
      g_overrideMaxSharedPresetSlotIndex.load(std::memory_order_relaxed);
  summary.maxLocalPointSlotIndex =
      g_overrideMaxLocalPointSlotIndex.load(std::memory_order_relaxed);
  summary.maxObservedChildLinkCount =
      g_overrideMaxObservedChildLinkCount.load(std::memory_order_relaxed);
  summary.maxObservedChildLinkTag =
      g_overrideMaxObservedChildLinkTag.load(std::memory_order_relaxed);
  summary.lastPrimaryPresetHash =
      g_overrideLastPrimaryPresetHash.load(std::memory_order_relaxed);
  summary.lastSharedPresetHash =
      g_overrideLastSharedPresetHash.load(std::memory_order_relaxed);
  summary.lastRuntimeModelPtr =
      g_overrideLastRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastMatchedChildRuntimeModelPtr =
      g_overrideLastMatchedChildRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastMatchedChildBySourceRecordRuntimeModelPtr =
      g_overrideLastMatchedChildBySourceRecordRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastContextRuntimeWithChildLinksPtr =
      g_overrideLastContextRuntimeWithChildLinksPtr.load(
          std::memory_order_relaxed);
  summary.lastScratchRootPtr =
      g_overrideLastScratchRootPtr.load(std::memory_order_relaxed);
  summary.lastScratchRootRuntimeModelPtr =
      g_overrideLastScratchRootRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastArgBlockPtr =
      g_overrideLastArgBlockPtr.load(std::memory_order_relaxed);
  summary.lastArgBlockRuntimeModelPtr =
      g_overrideLastArgBlockRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastArgBlockIdentityHintPtr =
      g_overrideLastArgBlockIdentityHintPtr.load(std::memory_order_relaxed);
  summary.lastArg4BlockPtr =
      g_overrideLastArg4BlockPtr.load(std::memory_order_relaxed);
  summary.lastArg4BlockRuntimeModelPtr =
      g_overrideLastArg4BlockRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastArg4BlockIdentityHintPtr =
      g_overrideLastArg4BlockIdentityHintPtr.load(std::memory_order_relaxed);
  summary.lastChildSourceMetaPtr =
      g_overrideLastChildSourceMetaPtr.load(std::memory_order_relaxed);
  summary.lastChildSourceMetaRuntimeModelPtr =
      g_overrideLastChildSourceMetaRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteBoundCandidateSpritePtr =
      g_overrideLastSpriteBoundCandidateSpritePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteBoundCandidateRuntimeModelPtr =
      g_overrideLastSpriteBoundCandidateRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastParentSpriteIdentityHintSpritePtr =
      g_overrideLastParentSpriteIdentityHintSpritePtr.load(
          std::memory_order_relaxed);
  summary.lastParentSpriteIdentityHintRuntimeModelPtr =
      g_overrideLastParentSpriteIdentityHintRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRootRuntimeModelPtr =
      g_overrideLastRootRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastSourceObjectRenderBridgeSourceObjectPtr =
      g_lastSourceObjectRenderBridgeSourceObjectPtr.load(
          std::memory_order_relaxed);
  summary.lastSourceObjectRenderBridgeSceneNodePtr =
      g_lastSourceObjectRenderBridgeSceneNodePtr.load(
          std::memory_order_relaxed);
  summary.lastSourceObjectIdentityHintSourceObjectPtr =
      g_lastSourceObjectIdentityHintSourceObjectPtr.load(
          std::memory_order_relaxed);
  summary.lastSourceObjectIdentityHintCandidatePtr =
      g_lastSourceObjectIdentityHintCandidatePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteHostSourceObjectPtr =
      g_lastSpriteHostSourceObjectPtr.load(std::memory_order_relaxed);
  summary.lastSpriteHostSpritePtr =
      g_lastSpriteHostSpritePtr.load(std::memory_order_relaxed);
  summary.lastSpriteHostRuntimeModelPtr =
      g_lastSpriteHostRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastSpriteHostUnitPtr =
      g_lastSpriteHostUnitPtr.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectPtr =
      g_lastSpriteFrameSourceObjectPtr.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceRuntimeModelPtr =
      g_lastSpriteFrameSourceRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceBaseRuntimeModelPtr =
      g_lastSpriteFrameSourceBaseRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectVtablePtr =
      g_lastSpriteFrameSourceObjectVtablePtr.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectSceneNodeCandidatePtr =
      g_lastSpriteFrameSourceObjectSceneNodeCandidatePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectSpriteCandidatePtr =
      g_lastSpriteFrameSourceObjectSpriteCandidatePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr =
      g_lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectRegistryFieldCandidatePtr =
      g_lastSpriteFrameSourceObjectRegistryFieldCandidatePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceDeepIdentityCandidatePtr =
      g_lastSpriteFrameSourceDeepIdentityCandidatePtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceWorldObjectEntryPtr =
      g_lastSpriteFrameSourceWorldObjectEntryPtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceSceneNodePtr =
      g_lastSpriteFrameSourceSceneNodePtr.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceUnitPtr =
      g_lastSpriteFrameSourceUnitPtr.load(std::memory_order_relaxed);
  summary.lastSpriteFramePoseBaseRuntimeModelPtr =
      g_lastSpriteFramePoseBaseRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFramePoseBaseMatrixCount =
      g_lastSpriteFramePoseBaseMatrixCount.load(std::memory_order_relaxed);
  summary.lastSpriteFrameAttachmentSpritePtr =
      g_lastSpriteFrameAttachmentSpritePtr.load(std::memory_order_relaxed);
  summary.lastSpriteFrameAttachmentRuntimeModelPtr =
      g_lastSpriteFrameAttachmentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameAttachmentContextPtr =
      g_lastSpriteFrameAttachmentContextPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectInitOwnerWidgetPtr =
      g_lastAttachedEffectInitOwnerWidgetPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectInitChildSpritePtr =
      g_lastAttachedEffectInitChildSpritePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectInitChildRuntimeModelPtr =
      g_lastAttachedEffectInitChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectInitParentRuntimeModelPtr =
      g_lastAttachedEffectInitParentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectInitUnitPtr =
      g_lastAttachedEffectInitUnitPtr.load(std::memory_order_relaxed);
  summary.lastAttachedEffectDirectOwnerWidgetPtr =
      g_lastAttachedEffectDirectOwnerWidgetPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectDirectChildSpritePtr =
      g_lastAttachedEffectDirectChildSpritePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectDirectChildRuntimeModelPtr =
      g_lastAttachedEffectDirectChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectDirectUnitPtr =
      g_lastAttachedEffectDirectUnitPtr.load(std::memory_order_relaxed);
  summary.lastAttachModelToPointParentSpritePtr =
      g_lastAttachModelToPointParentSpritePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointChildSpritePtr =
      g_lastAttachModelToPointChildSpritePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointChildRuntimeModelPtr =
      g_lastAttachModelToPointChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointPromotedOwnerRuntimeModelPtr =
      g_lastAttachModelToPointPromotedOwnerRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr =
      g_lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointPromotedChildRuntimeModelPtr =
      g_lastAttachModelToPointPromotedChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointPromotedChildModelResourcePtr =
      g_lastAttachModelToPointPromotedChildModelResourcePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachModelToPointUnitPtr =
      g_lastAttachModelToPointUnitPtr.load(std::memory_order_relaxed);
  summary.lastAttachScopeParentSpritePtr =
      g_lastAttachScopeParentSpritePtr.load(std::memory_order_relaxed);
  summary.lastAttachScopeParentRuntimeModelPtr =
      g_lastAttachScopeParentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachScopeChildSpritePtr =
      g_lastAttachScopeChildSpritePtr.load(std::memory_order_relaxed);
  summary.lastAttachScopeChildRuntimeModelPtr =
      g_lastAttachScopeChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachScopeHitRuntimeModelPtr =
      g_lastAttachScopeHitRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastCurrentRenderIdentityWorldObjectEntryPtr =
      g_lastCurrentRenderIdentityWorldObjectEntryPtr.load(
          std::memory_order_relaxed);
  summary.lastCurrentRenderIdentitySceneNodePtr =
      g_lastCurrentRenderIdentitySceneNodePtr.load(std::memory_order_relaxed);
  summary.lastCurrentRenderIdentityUnitPtr =
      g_lastCurrentRenderIdentityUnitPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeSourceObjectPtr =
      g_lastRuntimeSourceObjectPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeSourceSpriteObjectPtr =
      g_lastRuntimeSourceSpriteObjectPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeSourceRuntimeModelPtr =
      g_lastRuntimeSourceRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeModelResolveRuntimeModelPtr =
      g_lastRuntimeModelResolveRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeModelResolveHandlePtr =
      g_lastRuntimeModelResolveHandlePtr.load(std::memory_order_relaxed);
  summary.lastRuntimeModelCreateRuntimeModelPtr =
      g_lastRuntimeModelCreateRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeModelCreateModelDataPtr =
      g_lastRuntimeModelCreateModelDataPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeModelInitRuntimeModelPtr =
      g_lastRuntimeModelInitRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeModelInitModelDataPtr =
      g_lastRuntimeModelInitModelDataPtr.load(std::memory_order_relaxed);
  summary.lastAttachmentRigidSourceObjectPtr =
      g_lastAttachmentRigidSourceObjectPtr.load(std::memory_order_relaxed);
  summary.lastAttachmentRigidSourceSpriteObjectPtr =
      g_lastAttachmentRigidSourceSpriteObjectPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildLinkBuildParentRuntimeModelPtr =
      g_lastRuntimeChildLinkBuildParentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildLinkBuildChildRuntimeModelPtr =
      g_lastRuntimeChildLinkBuildChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildLinkBuildModelDataPtr =
      g_lastRuntimeChildLinkBuildModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr =
      g_lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildTimeDirectParentModelDataPtr =
      g_lastRuntimeChildBuildTimeDirectParentModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildTimeDirectRuntimeModelPtr =
      g_lastRuntimeChildBuildTimeDirectRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildTimeDirectModelDataPtr =
      g_lastRuntimeChildBuildTimeDirectModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildTimeDirectModelResourcePtr =
      g_lastRuntimeChildBuildTimeDirectModelResourcePtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataParentRuntimeModelPtr =
      g_lastRuntimeChildBuildModelDataParentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataPtr =
      g_lastRuntimeChildBuildModelDataPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataGroupRecordsPtr =
      g_lastRuntimeChildBuildModelDataGroupRecordsPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataHeadPtr =
      g_lastRuntimeChildBuildModelDataHeadPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataLinkNodePtr =
      g_lastRuntimeChildBuildModelDataLinkNodePtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataChildModelDataPtr =
      g_lastRuntimeChildBuildModelDataChildModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataChildModelResourcePtr =
      g_lastRuntimeChildBuildModelDataChildModelResourcePtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherMatchedRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherMatchedRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherMatrixCount =
      g_lastRuntimeMatrixPublisherMatrixCount.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount =
      g_lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount =
      g_lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr =
      g_lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount =
      g_lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr =
      g_lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr =
      g_lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr =
      g_lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr =
      g_lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapParentRuntimeModelPtr =
      g_lastAttachmentChildLineageBootstrapParentRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapChildRuntimeModelPtr =
      g_lastAttachmentChildLineageBootstrapChildRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapParentModelDataPtr =
      g_lastAttachmentChildLineageBootstrapParentModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapChildModelDataPtr =
      g_lastAttachmentChildLineageBootstrapChildModelDataPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapChildModelResourcePtr =
      g_lastAttachmentChildLineageBootstrapChildModelResourcePtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentAncestorFromRuntimeModelPtr =
      g_lastAttachmentAncestorFromRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastAttachmentAncestorRuntimeModelPtr =
      g_lastAttachmentAncestorRuntimeModelPtr.load(
          std::memory_order_relaxed);
  summary.lastLocalPointSlotIndex =
      g_overrideLastLocalPointSlotIndex.load(std::memory_order_relaxed);
  summary.lastLocalPointSourceRecordIndex =
      g_overrideLastLocalPointSourceRecordIndex.load(
          std::memory_order_relaxed);
  summary.lastObservedChildLinkCount =
      g_overrideLastObservedChildLinkCount.load(std::memory_order_relaxed);
  summary.lastMatchedChildLinkCount =
      g_overrideLastMatchedChildLinkCount.load(std::memory_order_relaxed);
  summary.lastMatchedChildMatrixCount =
      g_overrideLastMatchedChildMatrixCount.load(std::memory_order_relaxed);
  summary.lastMatchedChildBySourceRecordLinkCount =
      g_overrideLastMatchedChildBySourceRecordLinkCount.load(
          std::memory_order_relaxed);
  summary.lastMatchedChildBySourceRecordMatrixCount =
      g_overrideLastMatchedChildBySourceRecordMatrixCount.load(
          std::memory_order_relaxed);
  summary.lastContextRuntimeWithChildLinksOffset =
      g_overrideLastContextRuntimeWithChildLinksOffset.load(
          std::memory_order_relaxed);
  summary.lastContextRuntimeWithChildLinksCount =
      g_overrideLastContextRuntimeWithChildLinksCount.load(
          std::memory_order_relaxed);
  summary.lastContextRuntimeWithChildLinksMaxTag =
      g_overrideLastContextRuntimeWithChildLinksMaxTag.load(
          std::memory_order_relaxed);
  summary.lastScratchRootRuntimeChildLinkCount =
      g_overrideLastScratchRootRuntimeChildLinkCount.load(
          std::memory_order_relaxed);
  summary.lastScratchRootRuntimeMaxTag =
      g_overrideLastScratchRootRuntimeMaxTag.load(
          std::memory_order_relaxed);
  summary.lastArgBlockRuntimeOffset =
      g_overrideLastArgBlockRuntimeOffset.load(std::memory_order_relaxed);
  summary.lastArgBlockRuntimeChildLinkCount =
      g_overrideLastArgBlockRuntimeChildLinkCount.load(
          std::memory_order_relaxed);
  summary.lastArgBlockRuntimeMaxTag =
      g_overrideLastArgBlockRuntimeMaxTag.load(std::memory_order_relaxed);
  summary.lastArgBlockIdentityHintOffset =
      g_overrideLastArgBlockIdentityHintOffset.load(
          std::memory_order_relaxed);
  summary.lastArg4BlockRuntimeOffset =
      g_overrideLastArg4BlockRuntimeOffset.load(std::memory_order_relaxed);
  summary.lastArg4BlockRuntimeChildLinkCount =
      g_overrideLastArg4BlockRuntimeChildLinkCount.load(
          std::memory_order_relaxed);
  summary.lastArg4BlockRuntimeMaxTag =
      g_overrideLastArg4BlockRuntimeMaxTag.load(std::memory_order_relaxed);
  summary.lastArg4BlockIdentityHintOffset =
      g_overrideLastArg4BlockIdentityHintOffset.load(
          std::memory_order_relaxed);
  summary.lastRootRuntimeChildLinkCount =
      g_overrideLastRootRuntimeChildLinkCount.load(
          std::memory_order_relaxed);
  summary.lastRootRuntimeMaxTag =
      g_overrideLastRootRuntimeMaxTag.load(std::memory_order_relaxed);
  summary.lastSpriteHostJHandle =
      g_lastSpriteHostJHandle.load(std::memory_order_relaxed);
  summary.lastSpriteHostRawcode =
      g_lastSpriteHostRawcode.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceJHandle =
      g_lastSpriteFrameSourceJHandle.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceRawcode =
      g_lastSpriteFrameSourceRawcode.load(std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectRuntimeFieldOffset =
      g_lastSpriteFrameSourceObjectRuntimeFieldOffset.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceObjectRegistryFieldOffset =
      g_lastSpriteFrameSourceObjectRegistryFieldOffset.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameSourceDeepIdentityOffset =
      g_lastSpriteFrameSourceDeepIdentityOffset.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameAttachmentRoleMask =
      g_lastSpriteFrameAttachmentRoleMask.load(std::memory_order_relaxed);
  summary.lastSpriteFrameAttachmentUpdateKind =
      g_lastSpriteFrameAttachmentUpdateKind.load(
          std::memory_order_relaxed);
  summary.lastSpriteFrameAttachmentCallerRva =
      g_lastSpriteFrameAttachmentCallerRva.load(
          std::memory_order_relaxed);
  summary.lastSourceObjectIdentityHintOffset =
      g_lastSourceObjectIdentityHintOffset.load(
          std::memory_order_relaxed);
  summary.lastAttachedEffectInitJHandle =
      g_lastAttachedEffectInitJHandle.load(std::memory_order_relaxed);
  summary.lastAttachedEffectInitRawcode =
      g_lastAttachedEffectInitRawcode.load(std::memory_order_relaxed);
  summary.lastAttachedEffectDirectJHandle =
      g_lastAttachedEffectDirectJHandle.load(std::memory_order_relaxed);
  summary.lastAttachedEffectDirectRawcode =
      g_lastAttachedEffectDirectRawcode.load(std::memory_order_relaxed);
  summary.lastAttachModelToPointJHandle =
      g_lastAttachModelToPointJHandle.load(std::memory_order_relaxed);
  summary.lastAttachModelToPointRawcode =
      g_lastAttachModelToPointRawcode.load(std::memory_order_relaxed);
  summary.lastAttachModelToPointAttachPointIndex =
      g_lastAttachModelToPointAttachPointIndex.load(
          std::memory_order_relaxed);
  summary.lastAttachScopeCallerRva =
      g_lastAttachScopeCallerRva.load(std::memory_order_relaxed);
  summary.lastAttachScopeHitRoleMask =
      g_lastAttachScopeHitRoleMask.load(std::memory_order_relaxed);
  summary.lastRuntimeModelCtorRuntimeModelPtr =
      g_lastRuntimeModelCtorRuntimeModelPtr.load(std::memory_order_relaxed);
  summary.lastRuntimeModelCtorCallerRva =
      g_lastRuntimeModelCtorCallerRva.load(std::memory_order_relaxed);
  summary.lastRuntimeModelCtorKind =
      g_lastRuntimeModelCtorKind.load(std::memory_order_relaxed);
  summary.lastRuntimeModelResolveCallerRva =
      g_lastRuntimeModelResolveCallerRva.load(std::memory_order_relaxed);
  summary.lastRuntimeModelCreateCallerRva =
      g_lastRuntimeModelCreateCallerRva.load(std::memory_order_relaxed);
  summary.lastRuntimeModelInitCallerRva =
      g_lastRuntimeModelInitCallerRva.load(std::memory_order_relaxed);
  summary.lastRuntimeChildLinkBuildSourceMeta =
      g_lastRuntimeChildLinkBuildSourceMeta.load(std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataPhase =
      g_lastRuntimeChildBuildModelDataPhase.load(std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataGroupCount =
      g_lastRuntimeChildBuildModelDataGroupCount.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataLinkCount =
      g_lastRuntimeChildBuildModelDataLinkCount.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataUnreadableLinkCount =
      g_lastRuntimeChildBuildModelDataUnreadableLinkCount.load(
          std::memory_order_relaxed);
  summary.lastRuntimeChildBuildModelDataSourceMeta =
      g_lastRuntimeChildBuildModelDataSourceMeta.load(
          std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherKind =
      g_lastRuntimeMatrixPublisherKind.load(std::memory_order_relaxed);
  summary.lastRuntimeMatrixPublisherRoleMask =
      g_lastRuntimeMatrixPublisherRoleMask.load(std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapSourceMeta =
      g_lastAttachmentChildLineageBootstrapSourceMeta.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapBucketIndex =
      g_lastAttachmentChildLineageBootstrapBucketIndex.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapModelDataLinkCount =
      g_lastAttachmentChildLineageBootstrapModelDataLinkCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapRuntimeLinkCount =
      g_lastAttachmentChildLineageBootstrapRuntimeLinkCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapStrictCandidateCount =
      g_lastAttachmentChildLineageBootstrapStrictCandidateCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapSourceCandidateCount =
      g_lastAttachmentChildLineageBootstrapSourceCandidateCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapBucketCandidateCount =
      g_lastAttachmentChildLineageBootstrapBucketCandidateCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapAllCandidateCount =
      g_lastAttachmentChildLineageBootstrapAllCandidateCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal =
      g_lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal.load(
          std::memory_order_relaxed);
  summary.lastAttachmentChildLineageBootstrapModelDataBucketCount =
      g_lastAttachmentChildLineageBootstrapModelDataBucketCount.load(
          std::memory_order_relaxed);
  summary.lastAttachmentAncestorDepth =
      g_lastAttachmentAncestorDepth.load(std::memory_order_relaxed);
  summary.lastLocalPointX = FloatFromBits(
      g_overrideLastLocalPointXBits.load(std::memory_order_relaxed));
  summary.lastLocalPointY = FloatFromBits(
      g_overrideLastLocalPointYBits.load(std::memory_order_relaxed));
  summary.lastLocalPointZ = FloatFromBits(
      g_overrideLastLocalPointZBits.load(std::memory_order_relaxed));
  // Phase 7.35 路径 1 诊断透传：把 Exact 查询的命中/miss 分桶镜像到
  // current-draw contract summary，使 AutoTest 一次拉取同时看到
  // capture 端 miss 分布和 submit 端 lag 分布。
  dxvk::war3::render::PublishCaptureExactQueryCounters(
      g_queryBlendedPaletteExactHitCount.load(std::memory_order_relaxed),
      g_queryBlendedPaletteBestEffortHitCount.load(std::memory_order_relaxed),
      g_queryBlendedPaletteRejectedSlotOverflowCount.load(
          std::memory_order_relaxed),
      g_queryBlendedPaletteRejectedInvalidEntryCount.load(
          std::memory_order_relaxed),
      g_queryBlendedPaletteRejectedFrameTagMismatchCount.load(
          std::memory_order_relaxed),
      g_queryBlendedPaletteRejectedShortResultCount.load(
          std::memory_order_relaxed));
  return summary;
}

bool QueryRuntimeParentLink(void* childRuntimeModelPtr,
                            RuntimeParentLinkQueryResult& out) {
  out = {};
  if (childRuntimeModelPtr == nullptr)
    return false;

  std::lock_guard<std::mutex> lock(g_runtimeParentLinkMutex);
  const auto it = g_runtimeParentLinks.find(childRuntimeModelPtr);
  if (it == g_runtimeParentLinks.end())
    return false;

  out.known = true;
  out.parentRuntimeModelPtr =
      uint64_t(reinterpret_cast<uintptr_t>(it->second.parentRuntimeModelPtr));
  out.sourceMeta = it->second.sourceMeta;
  out.bucketIndex = it->second.bucketIndex;
  out.lastSeenFrame = it->second.lastSeenFrame;
  return true;
}

} // namespace model
} // namespace war3
} // namespace dxvk
