#include "war3_hook_render.h"
#include "war3_hook_address_book.h"
#include "war3_hook_render_identity.h"
#include "war3_hook_install_util.h"
#include "war3_jass_native_invoke_x86.h"
#include "war3_hook_ui.h"
#include "war3_queue_takeover_policy.h"
#include "../../d3d9_war3_branding.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_hook.h"
#include "../../d3d9_device.h"
#include "../../jass/war3_game.h"
#include "../war3.h"

#include "../core/war3_internal_test_config.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_memory.h"
#include "../core/war3_runtime_profile.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../reimpl/war3_render_queue.h"
#include "../render/war3_native_renderer_probe.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_render_dispatcher.h"
#include "../render/war3_render_exec_batch.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_render_state.h"
#include "../render/war3_renderer.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../render/war3_visible_renderables.h"
#include "../platform/war3_runtime_bootstrap.h"
#include "../state/war3_render_state.h"
#include "../tools/war3_perf_monitor.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "../core/war3_events.h"
#include "../shader/war3_shader_manager.h"
#include "war3_stage_tag_map.h"

namespace dxvk {

// Globals used by tracking
extern std::atomic<bool> g_renderQueueGlobalsValid;
extern uint32_t *g_numOfElementsPtr;
extern uint32_t *g_numOfTransparentPtr;
extern void **g_batchArrayPtr;
extern uint32_t *g_sortedBatchCountPtr;
extern void **g_sortedBatchPtrs;
extern uint32_t *g_stateOptEnabledPtr;
extern uint32_t *g_stateCleanupPendingPtr;

extern dxvk::war3::reimpl::ItemComparatorFn g_renderQueueItemComparator;
extern dxvk::war3::reimpl::ApplyStateBlockFn g_renderQueueApplyStateBlock;
extern dxvk::war3::reimpl::StageUpdateFn g_renderQueueStageUpdate;
extern dxvk::war3::reimpl::GxCleanupFn g_renderQueueGxCleanup74;
extern dxvk::war3::reimpl::GxCleanupFn g_renderQueueGxCleanup78;

} // namespace dxvk

namespace dxvk::war3::hooks {

using RenderDispatcherFn = int(__fastcall *)(int, int, int, int, int, int);
using WorldFrameUpdateAndPreparePassesFn = int(__fastcall *)(void *, void *, int,
                                                             int, int);
using WorldRenderSceneFn = int(__fastcall *)(void *, void *);
using SceneSubmitBatchFn = int(__fastcall *)(void *, void *, int, int, int,
                                             void *);
using WorldDispatchFn = int(__fastcall *)(void *, void *, int, int, int, int);
using WorldObjectsRenderGroupFn = int(__fastcall *)(void *, void *, int);
using DispatchCommonFn = int(__fastcall *)(void *, void *, void *, void *,
                                           void *);
using DispatchSpecialFn = int(__fastcall *)(void *, void *, void *, void *);
using TerrainRenderAllTilesFn = void(__fastcall *)(void *, void *);
using FlushSortedItemsFn = int(__cdecl *)();
using FlushTransparentFn = int(__cdecl *)();
using TransparentDispatchType0Fn = void(__fastcall *)(uint32_t, void *);
using TransparentDispatchTypeXFn = void(__fastcall *)(void *);

static RenderDispatcherFn g_originalRenderDispatcher = nullptr;
static RenderDispatcherFn g_trampolineRenderDispatcher = nullptr;

static WorldFrameUpdateAndPreparePassesFn g_originalWorldFrameUpdate = nullptr;
static WorldFrameUpdateAndPreparePassesFn g_trampolineWorldFrameUpdate =
    nullptr;

static WorldRenderSceneFn g_originalWorldRenderScene = nullptr;
static WorldRenderSceneFn g_trampolineWorldRenderScene = nullptr;

static SceneSubmitBatchFn g_originalSceneSubmitBatch = nullptr;
static SceneSubmitBatchFn g_trampolineSceneSubmitBatch = nullptr;

static WorldDispatchFn g_originalWorldDispatch = nullptr;
static WorldDispatchFn g_trampolineWorldDispatch = nullptr;

static WorldObjectsRenderGroupFn g_originalWorldObjectsRenderGroup = nullptr;
static WorldObjectsRenderGroupFn g_trampolineWorldObjectsRenderGroup = nullptr;

static DispatchCommonFn g_originalExecBatch0 = nullptr;
static DispatchCommonFn g_trampolineDispatchCommon = nullptr;

static DispatchSpecialFn g_originalExecBatch3 = nullptr;
static DispatchSpecialFn g_trampolineDispatchSpecial = nullptr;

static TerrainRenderAllTilesFn g_originalTerrainAllTiles = nullptr;
static TerrainRenderAllTilesFn g_trampolineTerrainAllTiles = nullptr;

static FlushSortedItemsFn g_originalFlushSortedItems = nullptr;
static FlushSortedItemsFn g_trampolineFlushSortedItems = nullptr;
static FlushTransparentFn g_originalFlushTransparent = nullptr;

static void **g_transparentArrayBasePtr = nullptr;
static void **g_transparentSortedPtrs = nullptr;
static TransparentDispatchType0Fn g_transparentDispatchType0 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType1 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType2 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType3 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType4 = nullptr;

#define WAR3_HOOK_HOTPATH_LOG(fmt, ...)                                        \
  do {                                                                         \
    if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {    \
      WAR3_RENDER_LOG(fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

static void MergeRenderObjectIdentity(
    dxvk::war3::render::RenderObjectIdentitySnapshot& dst,
    const dxvk::war3::render::RenderObjectIdentitySnapshot& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.agentPtr == nullptr)
    dst.agentPtr = src.agentPtr;
  if (dst.handleId == 0u)
    dst.handleId = src.handleId;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
  if (dst.agentType == 0u)
    dst.agentType = src.agentType;
  if (dst.flags5C == 0u)
    dst.flags5C = src.flags5C;
  if (dst.kind == dxvk::war3::render::ObjectKind::Unknown)
    dst.kind = src.kind;
  if (dst.groupIdx < 0)
    dst.groupIdx = src.groupIdx;
}

static void MergeShadowSemanticTlsIdentity(
    void* renderablePart,
    void* sceneNode,
    dxvk::war3::render::RenderObjectIdentitySnapshot& identity) {
  const auto& semantic = War3RenderState::GetTlsShadowSemanticState();
  if (!semantic.HasAnyContext())
    return;
  if (semantic.renderablePart != nullptr &&
      renderablePart != nullptr &&
      semantic.renderablePart != renderablePart) {
    return;
  }
  if (semantic.sceneNode != nullptr && sceneNode != nullptr &&
      semantic.sceneNode != sceneNode) {
    return;
  }

  if (identity.worldObjectEntry == nullptr)
    identity.worldObjectEntry = semantic.worldObjectEntry;
  if (identity.sceneNode == nullptr)
    identity.sceneNode = semantic.sceneNode;
  if (identity.jHandle == 0u)
    identity.jHandle = semantic.jHandle;
  if (identity.rawcode == 0u)
    identity.rawcode = semantic.rawcode;
  if (identity.kind == dxvk::war3::render::ObjectKind::Unknown)
    identity.kind = semantic.objectKind;
  if (identity.groupIdx < 0 && semantic.tag == War3BatchTag::WorldObjects)
    identity.groupIdx = 0;
  if (semantic.object != nullptr) {
    auto fromObject =
        dxvk::war3::render::MakeRenderObjectIdentitySnapshot(*semantic.object);
    MergeRenderObjectIdentity(identity, fromObject);
  }
}

static void TryFillUnitIdentityFromWorldObjectEntry(
    dxvk::war3::render::RenderObjectIdentitySnapshot& identity) {
  // entry+0 is not an authoritative CUnit in the current semantic path.  The
  // previous fallback promoted random object memory into unit flags and marked
  // most visible records as buildings, which produced construction/scaffold
  // shadow silhouettes.
  return;

  if (identity.worldObjectEntry == nullptr)
    return;

  void* objectPtr = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(identity.worldObjectEntry, 0u, objectPtr) ||
      objectPtr == nullptr) {
    return;
  }
  if (!dxvk::war3::IsReadableRangeFast(objectPtr, 0x64))
    return;

  uint32_t rawcode = 0u;
  uint32_t flags5C = 0u;
  dxvk::war3::SafeReadU32Fast(objectPtr, dxvk::war3::CUnitOffsets::Rawcode,
                              rawcode);
  dxvk::war3::SafeReadU32Fast(objectPtr, dxvk::war3::CUnitOffsets::Flags5C,
                              flags5C);
  if (rawcode == 0u)
    return;

  if (identity.unitPtr == nullptr)
    identity.unitPtr = objectPtr;
  if (identity.rawcode == 0u)
    identity.rawcode = rawcode;
  if (identity.flags5C == 0u)
    identity.flags5C = flags5C;
  if (identity.kind == dxvk::war3::render::ObjectKind::Unknown) {
    identity.kind =
        (flags5C & dxvk::war3::UnitFlags5C::Building) != 0u
            ? dxvk::war3::render::ObjectKind::Building
            : dxvk::war3::render::ObjectKind::Unit;
  }

  uint32_t handle0C = 0u;
  uint32_t handle10 = 0u;
  if (identity.jHandle == 0u &&
      dxvk::war3::SafeReadU32Fast(objectPtr,
                                  dxvk::war3::CUnitOffsets::HashId0C,
                                  handle0C) &&
      dxvk::war3::SafeReadU32Fast(objectPtr,
                                  dxvk::war3::CUnitOffsets::HashId10,
                                  handle10) &&
      handle0C != 0u && handle0C < 0x100000u && handle0C == handle10) {
    identity.handleId = handle0C;
    identity.jHandle = 0x100000u | handle0C;
  }
}

static void TryFillUnitIdentityFromUnitPtr(
    dxvk::war3::render::RenderObjectIdentitySnapshot& identity) {
  struct UnitIdentityHotCacheEntry {
    void* unitPtr = nullptr;
    uint32_t rawcode = 0u;
    uint32_t flags5C = 0u;
    uint32_t handleId = 0u;
    uint32_t jHandle = 0u;
    dxvk::war3::render::ObjectKind kind =
        dxvk::war3::render::ObjectKind::Unknown;
    bool valid = false;
  };

  auto mergeCachedIdentity =
      [](dxvk::war3::render::RenderObjectIdentitySnapshot& dst,
         const UnitIdentityHotCacheEntry& cached) {
        if (dst.rawcode == 0u)
          dst.rawcode = cached.rawcode;
        if (dst.flags5C == 0u)
          dst.flags5C = cached.flags5C;
        if (dst.kind == dxvk::war3::render::ObjectKind::Unknown)
          dst.kind = cached.kind;
        if (dst.jHandle == 0u && cached.jHandle != 0u) {
          dst.handleId = cached.handleId;
          dst.jHandle = cached.jHandle;
        }
      };

  if (identity.unitPtr == nullptr)
    return;

  static thread_local std::array<UnitIdentityHotCacheEntry, 1024>
      s_unitIdentityHotCache = {};
  const uintptr_t key = reinterpret_cast<uintptr_t>(identity.unitPtr);
  auto& cacheEntry = s_unitIdentityHotCache[(key >> 4u) &
                                            (s_unitIdentityHotCache.size() -
                                             1u)];
  if (cacheEntry.valid && cacheEntry.unitPtr == identity.unitPtr) {
    mergeCachedIdentity(identity, cacheEntry);
    return;
  }

  if (!dxvk::war3::IsReadableRangeFast(identity.unitPtr, 0x64))
    return;

  const auto* unitBytes = static_cast<const uint8_t*>(identity.unitPtr);
  uint32_t rawcode = 0u;
  uint32_t flags5C = 0u;
  uint32_t handle0C = 0u;
  uint32_t handle10 = 0u;
  std::memcpy(&rawcode, unitBytes + dxvk::war3::CUnitOffsets::Rawcode,
              sizeof(rawcode));
  std::memcpy(&flags5C, unitBytes + dxvk::war3::CUnitOffsets::Flags5C,
              sizeof(flags5C));
  std::memcpy(&handle0C, unitBytes + dxvk::war3::CUnitOffsets::HashId0C,
              sizeof(handle0C));
  std::memcpy(&handle10, unitBytes + dxvk::war3::CUnitOffsets::HashId10,
              sizeof(handle10));
  if (rawcode == 0u)
    return;

  cacheEntry.unitPtr = identity.unitPtr;
  cacheEntry.rawcode = rawcode;
  cacheEntry.flags5C = flags5C;
  cacheEntry.kind =
      (flags5C & dxvk::war3::UnitFlags5C::Building) != 0u
          ? dxvk::war3::render::ObjectKind::Building
          : dxvk::war3::render::ObjectKind::Unit;
  cacheEntry.handleId = 0u;
  cacheEntry.jHandle = 0u;
  if (handle0C != 0u && handle0C < 0x100000u && handle0C == handle10) {
    cacheEntry.handleId = handle0C;
    cacheEntry.jHandle = 0x100000u | handle0C;
  }
  cacheEntry.valid = true;

  mergeCachedIdentity(identity, cacheEntry);
}

static void ClassifyWorldTagIdentity(
    War3BatchTag tag,
    dxvk::war3::render::RenderObjectIdentitySnapshot& identity) {
  if (tag == War3BatchTag::WorldObjects) {
    if (identity.kind == dxvk::war3::render::ObjectKind::Unknown)
      identity.kind = dxvk::war3::render::ObjectKind::Unit;
    if (identity.groupIdx < 0)
      identity.groupIdx = 0;
  }
}

static bool HasHotVisibleUnitIdentity(
    const dxvk::war3::render::RenderObjectIdentitySnapshot& identity) {
  return identity.unitPtr != nullptr &&
         identity.rawcode != 0u &&
         identity.kind != dxvk::war3::render::ObjectKind::Unknown &&
         identity.groupIdx >= 0;
}

static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookCpuScope(const char *name);

static void PublishVisibleRenderableFromDispatch(
    void* sceneNode,
    void* renderablePart,
    uint32_t layerIndex,
    void* layerState) {
  if (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled)
    return;
  if (!dxvk::war3::runtime::IsWar3RuntimeModuleEnabled(
          dxvk::war3::runtime::War3RuntimeModule::SemanticData) ||
      !dxvk::war3::internal::kWar3RuntimeConfigSemanticFrameRegistriesEffective)
    return;
  if (renderablePart == nullptr)
    return;

  void* meshData = nullptr;
  void* partSceneNode = nullptr;
  {
    auto scope =
        MakeRenderHookCpuScope("Hook_PublishVisible/ReadRenderablePart");
    dxvk::war3::SafeReadPtrFast(
        renderablePart, dxvk::war3::RenderablePartFieldOffsets::MeshData,
        meshData);

    dxvk::war3::SafeReadPtrFast(
        renderablePart, dxvk::war3::RenderablePartFieldOffsets::SceneNode,
        partSceneNode);
    if (sceneNode == nullptr)
      sceneNode = partSceneNode;
  }

  if (sceneNode == nullptr && meshData == nullptr)
    return;

  const War3BatchTag tag = War3RenderState::GetTlsBatchTag();
  dxvk::war3::render::RenderObjectIdentitySnapshot identity = {};
  identity.sceneNode = sceneNode;
  {
    auto scope = MakeRenderHookCpuScope("Hook_PublishVisible/ResolveIdentity");
    MergeShadowSemanticTlsIdentity(renderablePart, sceneNode, identity);
    ClassifyWorldTagIdentity(tag, identity);

    if (!HasHotVisibleUnitIdentity(identity)) {
      dxvk::war3::render::RenderObjectIdentitySnapshot resolvedIdentity = {};
      if (dxvk::war3::render::TryResolveCurrentRenderObjectIdentity(
              sceneNode, resolvedIdentity)) {
        MergeRenderObjectIdentity(identity, resolvedIdentity);
      }
      if (identity.sceneNode == nullptr)
        identity.sceneNode = sceneNode;
    }

    if (!HasHotVisibleUnitIdentity(identity)) {
      dxvk::war3::render::RenderObjectIdentitySnapshot cachedIdentity = {};
      if (dxvk::war3::render::RenderQueueTracker::instance()
              .GetCachedObjectIdentity(renderablePart, cachedIdentity)) {
        MergeRenderObjectIdentity(identity, cachedIdentity);
      }
    }

    if (!HasHotVisibleUnitIdentity(identity)) {
      dxvk::war3::render::VisibleRenderableRecord prior = {};
      if (dxvk::war3::render::VisibleRenderableRegistry::instance()
              .queryByRenderablePart(renderablePart, prior)) {
        MergeRenderObjectIdentity(identity, prior.identity);
      }
    }
  }
  TryFillUnitIdentityFromWorldObjectEntry(identity);
  {
    auto scope = MakeRenderHookCpuScope("Hook_PublishVisible/FillUnitIdentity");
    if (identity.rawcode == 0u || identity.flags5C == 0u ||
        identity.jHandle == 0u ||
        identity.kind == dxvk::war3::render::ObjectKind::Unknown) {
      TryFillUnitIdentityFromUnitPtr(identity);
    }
  }
  ClassifyWorldTagIdentity(tag, identity);

  dxvk::war3::render::VisibleRenderableRecord record = {};
  record.queueKind =
      dxvk::war3::render::VisibleRenderableQueueKind::MainQueue;
  record.payload = renderablePart;
  record.renderablePart = renderablePart;
  record.sceneNode = sceneNode;
  record.meshData = meshData;
  record.layerState = layerState;
  record.layerIndex = layerIndex;
  record.identity = identity;
  {
    auto scope = MakeRenderHookCpuScope("Hook_PublishVisible/Register");
    dxvk::war3::render::VisibleRenderableRegistry::instance()
        .registerSemanticCandidate(record);
  }
}

static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookCpuScope(const char *name) {
  if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
    return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

static bool TryFetchNativeTimeOfDay(float &outTimeOfDay) {
  if constexpr (!dxvk::war3::internal::kNativeDirectGetFloatGameStateRuntimeEnabled) {
    return false;
  }

  static void *s_getFloatGameStateFn = nullptr;
  static bool s_loggedSuccess = false;
  static bool s_loggedFailure = false;
  static bool s_loggedMissingRuntime = false;
  static bool s_loggedMissingFn = false;
  static bool s_loggedInvokeUnsupported = false;

  if (!::game_war3) {
    if (!s_loggedMissingRuntime) {
      s_loggedMissingRuntime = true;
      war3dbg::Print(
          "DXVK War3Hook[Time]: native GetFloatGameState skipped (game_war3 not ready)\n");
    }
    return false;
  }

  if (!s_getFloatGameStateFn && ::pGameDLL) {
    const auto &book = GetWar3HookAddressBook127a();
    s_getFloatGameStateFn =
        reinterpret_cast<void *>(::pGameDLL + book.nativeGetFloatGameState);
  }

  if (!s_getFloatGameStateFn) {
    if (!s_loggedMissingFn) {
      s_loggedMissingFn = true;
      war3dbg::Print(
          "DXVK War3Hook[Time]: native GetFloatGameState skipped (fn unavailable)\n");
    }
    return false;
  }
  if (!IsCdeclPackedInvokeSupported()) {
    if (!s_loggedInvokeUnsupported) {
      s_loggedInvokeUnsupported = true;
      war3dbg::Print(
          "DXVK War3Hook[Time]: native GetFloatGameState skipped (cdecl packed invoke unsupported)\n");
    }
    return false;
  }

  const uint32_t arg = dxvk::war3::internal::kNativeGameStateTimeOfDayArg;
  const uint32_t rawValue =
      static_cast<uint32_t>(InvokeCdeclPacked(s_getFloatGameStateFn, &arg, 1));
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(rawValue));
  std::memcpy(&value, &rawValue, sizeof(value));
  if (!std::isfinite(value) || value < -0.5f || value > 24.5f) {
    if (!s_loggedFailure) {
      s_loggedFailure = true;
      war3dbg::Print(
          "DXVK War3Hook[Time]: native GetFloatGameState returned invalid value=%.6f raw=%08X arg=%u\n",
          static_cast<double>(value), static_cast<unsigned>(rawValue),
          static_cast<unsigned>(arg));
    }
    return false;
  }

  outTimeOfDay = value;
  if (!s_loggedSuccess) {
    s_loggedSuccess = true;
    war3dbg::Print(
        "DXVK War3Hook[Time]: native GetFloatGameState first successful retrieval: %.6f\n",
        static_cast<double>(value));
  }
  return true;
}

struct BatchTagStageScopeLite {
  War3BatchTag prevTag = War3BatchTag::Unknown;
  int prevStage = -1;
  bool stageOverridden = false;
  War3TlsShadowSemanticState prevShadowSemantic = {};

  void begin(War3BatchTag tag, int stage) {
    prevTag = War3RenderState::GetCurrentBatchTag();
    prevStage = War3RenderState::GetStage();
    prevShadowSemantic = War3RenderState::GetTlsShadowSemanticState();
    stageOverridden = stage >= 0;
    War3RenderState::SetBatchTag(tag);
    if (stageOverridden)
      War3RenderState::SetStage(stage);
    War3RenderState::SetTlsBatchHandle(0);
    War3RenderState::ClearTlsShadowSemanticState();
    dxvk::war3::render::SetCurrentBatchObject(nullptr);
  }

  void end() const {
    if (stageOverridden)
      War3RenderState::SetStage(prevStage);
    War3RenderState::SetBatchTag(prevTag);
    War3RenderState::SetTlsBatchHandle(0);
    War3RenderState::SetTlsShadowSemanticState(prevShadowSemantic);
    dxvk::war3::render::SetCurrentBatchObject(nullptr);
  }
};

struct GroupBatchTagScope {
  War3BatchTag prevTag = War3BatchTag::Unknown;
  bool active = false;

  void begin(War3BatchTag tag) {
    prevTag = War3RenderState::GetTlsBatchTag();
    active = true;
    War3RenderState::SetBatchTag(tag);
  }

  void end() const {
    if (active)
      War3RenderState::SetBatchTag(prevTag);
  }
};

static void TrackRenderQueueUpdates(int stage, uint32_t before,
                                    void *initialBatchArray) {
  // 在 WorldDispatch 前后比较队列长度，给新增元素补齐 stage/tag 映射。
  // 该步骤是后续 ExecBatch 精确桥接的基础。
  if (!g_renderQueueGlobalsValid.load(std::memory_order_relaxed) ||
      !g_numOfElementsPtr || !g_batchArrayPtr)
    return;

  const std::uint32_t after = *g_numOfElementsPtr;
  void *batchArrayCurrent = *g_batchArrayPtr;
  void *batchArray = batchArrayCurrent ? batchArrayCurrent : initialBatchArray;

  if (!batchArray || after <= before)
    return;

  constexpr std::uint32_t kMaxElements = 250000;
  if (after > kMaxElements) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      war3dbg::Print("DXVK War3Hook: RenderQueue Overflow (stage=%d after=%u), "
                     "disabling tracker.\n",
                     stage, after);
    }
    g_renderQueueGlobalsValid.store(false, std::memory_order_relaxed);
    g_numOfElementsPtr = nullptr;
    g_batchArrayPtr = nullptr;
    return;
  }

  constexpr uint64_t kElemStride = 20ull;
  const uint64_t startPos = static_cast<uint64_t>(before) * kElemStride;
  const uint64_t size = static_cast<uint64_t>(after - before) * kElemStride;
  const uint64_t requiredBytes = static_cast<uint64_t>(after) * kElemStride;

  bool isRangeReadable = false;
  if constexpr (dxvk::war3::internal::
                    kNativeTrackQueueReadableRangeCacheEnabled) {
    static void *s_cachedBatchArray = nullptr;
    static uint64_t s_cachedReadableBytes = 0;
    static bool s_cachedReadable = false;

    if (batchArray != s_cachedBatchArray ||
        requiredBytes > s_cachedReadableBytes) {
      s_cachedBatchArray = batchArray;
      s_cachedReadableBytes = requiredBytes;
      s_cachedReadable =
          IsReadableRange(reinterpret_cast<const void *>(batchArray),
                          static_cast<size_t>(requiredBytes));
    }
    isRangeReadable =
        s_cachedReadable && (startPos + size <= s_cachedReadableBytes);
  } else {
    const auto *checkPtr =
        reinterpret_cast<std::uint8_t *>(batchArray) + startPos;
    const size_t checkSize = static_cast<size_t>(size);
    isRangeReadable = IsReadableRange(checkPtr, checkSize);
  }

  if (isRangeReadable) {
    auto &tracker = dxvk::war3::render::RenderQueueTracker::instance();
    tracker.MarkStages(batchArray, before, after, stage);

    War3BatchTag tag = dxvk::war3::hooks::MapStageToTag(
        stage, dxvk::war3::internal::kNativeStageTagProfile);

    if (dxvk::war3::hooks::ShouldSuppressStageTagByGroupMode(
            stage, dxvk::war3::internal::kNativeStageTagProfile,
            dxvk::war3::internal::kNativeTagWorldByGroupIdx)) {
      tag = War3BatchTag::Unknown;
    }

    if (tag != War3BatchTag::Unknown) {
      tracker.MarkTags(batchArray, before, after, tag);
    }
  } else {
    g_renderQueueGlobalsValid.store(false, std::memory_order_relaxed);
  }
}

static void TryNativeSemanticWorldStageValidation(int stage, int a3, int a4,
                                                  int a5) {
  if constexpr (!dxvk::war3::internal::kNativeRendererHostExecuteValidationEnabled ||
                !dxvk::war3::internal::
                    kNativeSemanticShadowWorldStageValidationEnabled) {
    return;
  } else {
    if (!dxvk::war3::internal::
            IsNativeRendererHostExecuteValidationRuntimeEnabled() ||
        !dxvk::war3::internal::
            IsNativeSemanticShadowWorldStageValidationRuntimeEnabled()) {
      return;
    }

    const bool isTargetStage =
        stage == dxvk::war3::internal::kNativeSemanticShadowPrepareStage ||
        stage ==
            dxvk::war3::internal::kNativeSemanticShadowRefreshPrepareStage ||
        stage == dxvk::war3::internal::kNativeSemanticShadowExecuteStage;
    if (!isTargetStage)
      return;

    const bool jassReady = dxvk::war3::War3Events::get().isJassReady();
    const bool gameStarted = dxvk::war3::War3Events::get().isGameStarted();
    dxvk::war3::render::NoteNativeSemanticWorldStageCandidate(
        stage, a3, a4, a5, jassReady, gameStarted);

    // CWorld stage 2/10/11 can appear before the synthetic OnGameStart marker
    // on fast-loading visual maps. Keep that early validation behind an
    // explicit runtime switch so normal semantic preview cannot stall map-ready.
    const bool semanticRuntimeReady =
        gameStarted ||
        (jassReady && dxvk::war3::internal::
                          IsSemanticShadowPreReadyValidationRuntimeEnabled());
    if (!semanticRuntimeReady) {
      dxvk::war3::render::NoteNativeSemanticWorldStageSkippedRuntimeNotReady(
          stage);
      return;
    }

    if (stage == dxvk::war3::internal::kNativeSemanticShadowPrepareStage ||
        stage ==
            dxvk::war3::internal::kNativeSemanticShadowRefreshPrepareStage) {
      auto scope = MakeRenderHookCpuScope(
          "Hook_WorldDispatch/NativeSemanticPrepareStage");
      const bool refreshStage =
          stage == dxvk::war3::internal::
                       kNativeSemanticShadowRefreshPrepareStage;
      const bool prepared = dxvk::war3::platform::DriveNativeShadowBackend(
          true, refreshStage ? 8u : 4u);
      dxvk::war3::render::NoteNativeSemanticWorldStagePrepare(stage,
                                                              prepared);
      return;
    }

    if (stage ==
        dxvk::war3::internal::kNativeSemanticShadowExecuteStage) {
      if constexpr (dxvk::war3::internal::
                        kShadowSemanticCoreSceneSubmissionEnabled) {
        if (dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
          if (auto* device = dxvk::war3::GetActiveDevice()) {
            device->War3PopulateSemanticShadowSceneForValidation(
                dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly,
                false);
          }
        }
      }
      {
        auto prepareScope = MakeRenderHookCpuScope(
            "Hook_WorldDispatch/NativeSemanticFinalPrepareStage");
        const bool prepared =
            dxvk::war3::platform::DriveNativeShadowBackend(
                true, 32u);
        dxvk::war3::render::NoteNativeSemanticWorldStagePrepare(stage,
                                                                prepared);
        if (!prepared)
          return;
      }

      auto scope = MakeRenderHookCpuScope(
          "Hook_WorldDispatch/NativeSemanticExecuteStage");
      const bool executed =
          dxvk::war3::platform::ExecuteNativeShadowBackendPreparedFrame();
      dxvk::war3::render::NoteNativeSemanticWorldStageExecute(stage,
                                                              executed);
    }
  }
}

int __fastcall Hook_RenderDispatcher(int ctx1, int ctx2, int typeCode,
                                     int stage, int a5, int dataStore) {
  // 负责维护 dispatcher stage 栈，保证递归分发时阶段上下文不串台。
  auto perfScope = MakeRenderHookCpuScope("Hook_RenderDispatcher");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_RENDER_LOG("DXVK War3Hook: Hook_RenderDispatcher FIRST_CALL ctx1=0x%X "
                    "ctx2=0x%X typeCode=0x%X stage=%d a5=0x%X dataStore=0x%X\n",
                    ctx1, ctx2, typeCode, stage, a5, dataStore);
  }

  const int prevDispatcherStage =
      dxvk::war3::render::War3RenderDispatcher::instance().PushDispatcherStage(
          stage);
  int result = 0;
  if (g_trampolineRenderDispatcher) {
    auto origScope = MakeRenderHookCpuScope("Hook_RenderDispatcher/Orig");
    result = g_trampolineRenderDispatcher(ctx1, ctx2, typeCode, stage, a5,
                                          dataStore);
  }
  dxvk::war3::render::War3RenderDispatcher::instance().PopDispatcherStage(
      prevDispatcherStage);
  return result;
}

int __fastcall Hook_WorldFrameUpdateAndPreparePasses(void *thisPtr, void *edx,
                                                     int a2, int a3, int a4) {
  auto perfScope = MakeRenderHookCpuScope("Hook_WorldFramePrepare");

  if constexpr (!dxvk::war3::internal::kNativeWorldFrameBoundaryHooksEnabled) {
    if (g_trampolineWorldFrameUpdate)
      return g_trampolineWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
    if (g_originalWorldFrameUpdate)
      return g_originalWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
    return 0;
  }

  War3RenderState::OnWorldFramePrepareEnter();
  int result = 0;
  if (g_trampolineWorldFrameUpdate) {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldFramePrepare/Orig");
    result = g_trampolineWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
  } else if (g_originalWorldFrameUpdate) {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldFramePrepare/Orig");
    result = g_originalWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
  }
  War3RenderState::OnWorldFramePrepareExit();
  return result;
}

int __fastcall Hook_WorldRenderScene(void *thisPtr, void *edx) {
  auto perfScope = MakeRenderHookCpuScope("Hook_WorldRenderScene");

  if constexpr (!dxvk::war3::internal::kNativeWorldFrameBoundaryHooksEnabled) {
    if (g_trampolineWorldRenderScene)
      return g_trampolineWorldRenderScene(thisPtr, edx);
    if (g_originalWorldRenderScene)
      return g_originalWorldRenderScene(thisPtr, edx);
    return 0;
  }

  War3RenderState::OnWorldRenderSceneEnter();
  int result = 0;
  if (g_trampolineWorldRenderScene) {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldRenderScene/Orig");
    result = g_trampolineWorldRenderScene(thisPtr, edx);
  } else if (g_originalWorldRenderScene) {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldRenderScene/Orig");
    result = g_originalWorldRenderScene(thisPtr, edx);
  }
  War3RenderState::OnWorldRenderSceneExit();
  return result;
}

int __fastcall Hook_SceneSubmitBatch(void *thisPtr, void *edx, int a2, int a3,
                                     int stage, void *entries) {
  // SceneSubmitBatch 侧主要做阶段上下文桥接与同步诊断，不修改原提交语义。
  auto perfScope = MakeRenderHookCpuScope("Hook_SceneSubmitBatch");

  if constexpr (dxvk::war3::internal::kNativeSceneSubmitBatchCounterLogging) {
    static std::atomic<uint32_t> s_sbCounter{0};
    const uint32_t c = s_sbCounter++;
    if (c < 10 || c % 300 == 0) {
      WAR3_RENDER_LOG("DXVK War3Hook: Hook_SceneSubmitBatch CALL (c=%u) "
                      "this=0x%p stage=%d a2=0x%X a3=0x%X entries=0x%p\n",
                      c, thisPtr, stage, static_cast<unsigned>(a2),
                      static_cast<unsigned>(a3), entries);
    }
  }

  const int prevDispatcherStage =
      dxvk::war3::render::War3RenderDispatcher::instance().PushDispatcherStage(
          stage);

  void *currentEntry =
      dxvk::war3::render::War3Renderer::instance().GetCurrentWorldObjectEntry();
  if (currentEntry) {
    static std::set<void *> s_foundSyncBatches;
    if (s_foundSyncBatches.find(currentEntry) == s_foundSyncBatches.end() &&
        s_foundSyncBatches.size() < 20) {
      s_foundSyncBatches.insert(currentEntry);
      WAR3_RENDER_LOG(
          "DXVK War3Hook: [SYNC-BATCH] SceneSubmitBatch called inside "
          "WorldObjectEntry! Entry=0x%p Entries=0x%p Stage=%d\n",
          currentEntry, entries, stage);
    }
  }

  int result = 0;
  if (g_trampolineSceneSubmitBatch) {
    auto origScope = MakeRenderHookCpuScope("Hook_SceneSubmitBatch/Orig");
    result = g_trampolineSceneSubmitBatch(thisPtr, edx, a2, a3, stage, entries);
  } else if (g_originalSceneSubmitBatch) {
    auto origScope = MakeRenderHookCpuScope("Hook_SceneSubmitBatch/Orig");
    result = g_originalSceneSubmitBatch(thisPtr, edx, a2, a3, stage, entries);
  }

  dxvk::war3::render::War3RenderDispatcher::instance().PopDispatcherStage(
      prevDispatcherStage);
  return result;
}

int __fastcall Hook_WorldDispatch(void *thisPtr, void *edx, int stage, int a3,
                                  int a4, int a5) {
  // WorldDispatch 是渲染主时序入口：
  // 1) 同步 game time/全局 uniform；
  // 2) 记录 stage；
  // 3) 在调用原函数后追踪本阶段新增批次。
  auto perfScope = MakeRenderHookCpuScope("Hook_WorldDispatch");

  // FPS 覆盖仅需低频/幂等尝试；限制到 stage0 可避免在 WorldDispatch 热路径重复调用。
  if (stage == 0) {
    War3TryOverrideMaxFps();
  }

  using namespace dxvk::war3::internal;
  if (unlikely(kStageDebugEnabled) && stage >= 0 && stage < 25) {
    if (!kStageDebug[stage])
      return 0;
  }

  {
    auto timeSyncScope = MakeRenderHookCpuScope("Hook_WorldDispatch/TimeSync");
    if (stage == 0 || stage == 1) {
      // 说明：
      // - 优先直接调用原生 GetFloatGameState(GAME_STATE_TIME_OF_DAY)；
      // - 若 direct c_call 暂时拿不到合法值，再回退到本地时钟，避免时间链硬断。
      static auto s_lastUpdate = std::chrono::steady_clock::now();
      static auto s_gameClockStart = std::chrono::steady_clock::time_point{};
      static bool s_gameClockValid = false;
      const auto now = std::chrono::steady_clock::now();
      const bool runtimeReady =
          dxvk::war3::War3Events::get().isJassReady() && (::game_war3 != nullptr);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                s_lastUpdate)
              .count() >= 100) {
        s_lastUpdate = now;
        float gameTime = -1.0f;
        if (runtimeReady) {
          const bool usedNative = TryFetchNativeTimeOfDay(gameTime);

          if (!usedNative) {
            if (dxvk::war3::War3Events::get().isGameStarted()) {
              if (!s_gameClockValid) {
                s_gameClockValid = true;
                s_gameClockStart = now;
              }

              gameTime = static_cast<float>(
                  std::chrono::duration_cast<std::chrono::duration<double>>(
                      now - s_gameClockStart)
                      .count());
            } else {
              gameTime = -1.0f;
            }
          } else {
            s_gameClockValid = false;
          }
        } else {
          if (dxvk::war3::War3Events::get().isGameStarted()) {
            if (!s_gameClockValid) {
              s_gameClockValid = true;
              s_gameClockStart = now;
            }

            gameTime = static_cast<float>(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - s_gameClockStart)
                    .count());
          } else {
            s_gameClockValid = false;
            gameTime = -1.0f;
          }
        }

        if (gameTime >= 0.0f || dxvk::war3::War3Events::get().isGameStarted()) {
          War3RenderState::SetGameTime(gameTime);
          War3VKBranding::TryShowBrandingMessage(gameTime);
          war3::ShaderManager::get().setGlobalFloat4(
              "Time", Vector4(gameTime, 0.0f, 0.0f, 0.0f));
        } else {
          War3RenderState::SetGameTime(-1.0f);
        }
      }
    }
  }

  const bool needBatchTracking = War3RenderState::IsBatchTagTrackingEnabled();

  std::uint32_t before = 0;
  void *batchArray = nullptr;
  if (needBatchTracking &&
      g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
      g_numOfElementsPtr && g_batchArrayPtr) {
    before = *g_numOfElementsPtr;
    batchArray = *g_batchArrayPtr;
  }

  War3RenderState::SetStage(stage);

  int result = 0;
  {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldDispatch/Orig");
    if (g_trampolineWorldDispatch) {
      result = g_trampolineWorldDispatch(thisPtr, edx, stage, a3, a4, a5);
    } else if (g_originalWorldDispatch) {
      result = g_originalWorldDispatch(thisPtr, edx, stage, a3, a4, a5);
    }
  }

  if (needBatchTracking) {
    auto trackScope =
        MakeRenderHookCpuScope("Hook_WorldDispatch/TrackRenderQueueUpdates");
    TrackRenderQueueUpdates(stage, before, batchArray);
  }

  TryNativeSemanticWorldStageValidation(stage, a3, a4, a5);

  War3RenderState::OnStageExit(stage);
  if (a5 == 0) {
    War3RenderState::OnMainWorldStageExit(stage);
  }

  War3RenderState::SetStage(-1);
  return result;
}

int __fastcall Hook_WorldObjects_RenderGroup(void *thisPtr, void * /*edx*/,
                                             int groupIdx) {
  // WorldObjects group 入口负责对象收集与分组开关控制（Units/Buildings/Effects）。
  auto perfScope = MakeRenderHookCpuScope("Hook_WorldObjects_RenderGroup");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_HOOK_HOTPATH_LOG("DXVK War3Hook: Hook_WorldObjects_RenderGroup "
                          "FIRST_CALL this=0x%p groupIdx=%d\n",
                          thisPtr, groupIdx);
  }

  if (thisPtr) {
    dxvk::war3::state::RenderState::instance().setWorldPointer(thisPtr);
  }

  using namespace dxvk::war3::internal;
  static uint32_t s_shadowGroupLog = 0;

  bool shouldRender = true;
  {
    auto shouldRenderScope =
        MakeRenderHookCpuScope("Hook_WorldObjects_RenderGroup/ShouldRenderCheck");
    switch (groupIdx) {
    case 0: // Units
      shouldRender = kShadowRenderGroup0;
      if (s_shadowGroupLog < 5 && !shouldRender)
        WAR3_HOOK_HOTPATH_LOG("DXVK: Skipping Group 0 (Units) - disabled\n");
      break;
    case 1: // Buildings
      shouldRender = kShadowRenderGroup1;
      if (s_shadowGroupLog < 5 && !shouldRender)
        WAR3_HOOK_HOTPATH_LOG("DXVK: Skipping Group 1 (Buildings) - disabled\n");
      break;
    case 2: // Effects/Decorations
      shouldRender = kShadowRenderGroup2;
      if (s_shadowGroupLog < 5 && !shouldRender)
        WAR3_HOOK_HOTPATH_LOG("DXVK: Skipping Group 2 (Effects) - disabled\n");
      break;
    case 3: // ShadowCasters
      shouldRender = kShadowRenderGroup3;
      if (s_shadowGroupLog < 5 && !shouldRender)
        WAR3_HOOK_HOTPATH_LOG(
            "DXVK: Skipping Group 3 (ShadowCasters) - disabled\n");
      break;
    default:
      shouldRender = true;
      break;
    }
  }

  if (s_shadowGroupLog < 5) {
    s_shadowGroupLog++;
    WAR3_HOOK_HOTPATH_LOG(
        "DXVK: WorldObjects_RenderGroup groupIdx=%d shouldRender=%d G0=%d "
        "G1=%d G2=%d G3=%d\n",
        groupIdx, shouldRender ? 1 : 0, kShadowRenderGroup0 ? 1 : 0,
        kShadowRenderGroup1 ? 1 : 0, kShadowRenderGroup2 ? 1 : 0,
        kShadowRenderGroup3 ? 1 : 0);
  }

  if (!shouldRender)
    return 0;

  War3BatchTag groupTag = War3BatchTag::Unknown;
  switch (groupIdx) {
  case 0:
    groupTag = War3BatchTag::WorldObjects;
    break;
  case 1:
    groupTag = War3BatchTag::SelectionOverlay;
    break;
  case 2:
    groupTag = War3BatchTag::Decorations;
    break;
  default:
    break;
  }

  GroupBatchTagScope groupTagScope;
  if (groupTag != War3BatchTag::Unknown)
    groupTagScope.begin(groupTag);

  uint32_t beforeGroupTag = 0;
  bool shouldTrackGroupTag = false;
  if constexpr (dxvk::war3::internal::kNativeTagWorldByGroupIdx) {
    shouldTrackGroupTag =
        War3RenderState::IsBatchTagTrackingEnabled() &&
        g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
        g_numOfElementsPtr && g_batchArrayPtr;
    if (shouldTrackGroupTag)
      beforeGroupTag = *g_numOfElementsPtr;
  }

  const bool needsObjectTracking = War3RenderState::NeedsObjectTracking();
  const bool needsShadowObjectIdentity =
      War3RenderState::NeedsShadowObjectIdentity();
  const bool semanticSceneOwnsUnits =
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      dxvk::war3::internal::
          IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled() &&
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly;
  const bool pathBlockerTrackingOnly =
      needsObjectTracking && !needsShadowObjectIdentity &&
      !War3RenderState::HasOutlineHandles() &&
      !War3RenderState::HasBloomHandles() &&
      !War3RenderState::IsForceObjectTrackingEnabled() &&
      !War3RenderState::IsOutlineDebugAllObjectsEnabled() &&
      dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
      dxvk::war3::internal::kPathBlockerHideEnabled &&
      dxvk::war3::internal::kPathBlockerForceBridgeTrackingEnabled;
  const bool pathBlockerCollectThisGroup =
      !pathBlockerTrackingOnly ||
      ((dxvk::war3::internal::kPathBlockerTrackingGroupMask &
        (1u << uint32_t(groupIdx))) != 0u);
  const bool allowPathBlockerOnlyCollect =
      !semanticSceneOwnsUnits || needsShadowObjectIdentity;
  const bool needCollectObjects =
      dxvk::war3::internal::kNativeWorldGroupAlwaysCollectObjects ||
      needsShadowObjectIdentity ||
      dxvk::war3::render::NativeRendererProbe::IsEnabled() ||
      (needsObjectTracking &&
       (!pathBlockerTrackingOnly ||
        (allowPathBlockerOnlyCollect && pathBlockerCollectThisGroup)));
  if (needCollectObjects) {
    auto collectScope =
        MakeRenderHookCpuScope("Hook_WorldObjects_RenderGroup/CollectObjects");
    dxvk::war3::render::War3Renderer::instance().OnWorldObjectsGroup(thisPtr,
                                                                      groupIdx);
  }

  int result = 0;
  if (g_trampolineWorldObjectsRenderGroup) {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldObjects_RenderGroup/Orig");
    result = g_trampolineWorldObjectsRenderGroup(thisPtr, nullptr, groupIdx);
  } else if (g_originalWorldObjectsRenderGroup) {
    auto origScope = MakeRenderHookCpuScope("Hook_WorldObjects_RenderGroup/Orig");
    result = g_originalWorldObjectsRenderGroup(thisPtr, nullptr, groupIdx);
  }

  if constexpr (dxvk::war3::internal::kNativeTagWorldByGroupIdx) {
    if (shouldTrackGroupTag) {
      dxvk::war3::render::RenderQueueTracker::instance().TrackNewBatches(
          beforeGroupTag, groupIdx);
    }
  }

  groupTagScope.end();

  return result;
}

int __fastcall Hook_RenderQueue_Dispatch_Common(void *thisPtr, void *edx,
                                                void *a3, void *a4, void *a5) {
  // Common Dispatch 热路径策略：
  // - 无追踪需求时走快速直通；
  // - 需要追踪时走 ExecBatchProcessor 桥接并恢复状态。
  auto perfScope = MakeRenderHookCpuScope("Hook_Dispatch_Common");
  if constexpr (dxvk::war3::internal::kShadowSemanticDispatchContractProbeEnabled) {
    dxvk::war3::render::VisibleRenderableRecord visible = {};
    dxvk::war3::reimpl::RenderBatchElement syntheticBatch = {};
    syntheticBatch.renderablePart = edx;
    syntheticBatch.layerIndex = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a3));
    if (dxvk::war3::render::VisibleRenderableRegistry::instance()
            .queryByRenderablePart(edx, visible)) {
      syntheticBatch.layerStatePtr = visible.layerState;
      syntheticBatch.subIndex = visible.subIndex;
      dxvk::war3::reimpl::MaybeLogSemanticDispatchContract(
          &syntheticBatch, thisPtr != nullptr ? thisPtr : visible.sceneNode,
          visible.meshData);
    } else {
      dxvk::war3::reimpl::LogSemanticDispatchSkip(
          "dispatch-registry-miss", &syntheticBatch, thisPtr, nullptr, nullptr,
          dxvk::war3::render::kInvalidVisibleMeshIndex, 0u);
    }
  }

  War3BatchTag tag = War3RenderState::GetTlsBatchTag();
  int elementStage = -1;
  const bool needsObjectTracking = War3RenderState::NeedsObjectTracking();
  const bool needsShadowObjectIdentity =
      War3RenderState::NeedsShadowObjectIdentity();
  const bool needsShadowFallbackBridge =
      War3RenderState::NeedsShadowDrawFallbackBridge();
  const bool needsShadowSemanticTracking =
      War3RenderState::NeedsShadowSemanticTracking();
  const bool needsBatchTracking = War3RenderState::IsBatchTagTrackingEnabled();
  const bool needsProbe = dxvk::war3::render::NativeRendererProbe::IsEnabled();

  auto publishVisibleAfterDispatch = [&]() {
    if (!needsShadowSemanticTracking ||
        !dxvk::war3::hooks::IsWorldBridgeTag(tag))
      return;
    const uint32_t layerIndex =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a3));
    PublishVisibleRenderableFromDispatch(thisPtr, edx, layerIndex, nullptr);
  };

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeWhenNoTracking &&
      !needsObjectTracking && !needsShadowSemanticTracking &&
      !needsBatchTracking && !needsProbe) {
    const int res = g_trampolineDispatchCommon(thisPtr, edx, a3, a4, a5);
    publishVisibleAfterDispatch();
    War3RenderState::SetTlsDispatchHandle(0);
    War3RenderState::SetTlsBatchHandle(0);
    return res;
  }

  const bool canUseWorldGroupTagFastPath =
      needsShadowSemanticTracking && !needsObjectTracking &&
      !needsShadowObjectIdentity && !needsShadowFallbackBridge && !needsProbe &&
      dxvk::war3::hooks::IsWorldBridgeTag(tag);

  if ((!dxvk::war3::internal::kNativeHookFastPathEnabled ||
       needsObjectTracking || needsShadowSemanticTracking ||
       needsBatchTracking || needsProbe) &&
      !canUseWorldGroupTagFastPath) {
    auto &tracker = dxvk::war3::render::RenderQueueTracker::instance();
    tracker.GetTagStage(edx, tag, elementStage);
  }

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeForNonWorldTag &&
      (needsObjectTracking || needsShadowSemanticTracking) &&
      !needsBatchTracking && !needsProbe &&
      tag != War3BatchTag::Unknown &&
      !dxvk::war3::hooks::IsWorldBridgeTag(tag)) {
    const int res = g_trampolineDispatchCommon(thisPtr, edx, a3, a4, a5);
    publishVisibleAfterDispatch();
    War3RenderState::SetTlsDispatchHandle(0);
    War3RenderState::SetTlsBatchHandle(0);
    return res;
  }

  const bool keepBridgeForConservativeMerge =
      dxvk::war3::internal::kNativeConservativeMergedSubmitEnabled &&
      dxvk::war3::internal::
          kNativeConservativeMergedSubmitAllowNoObjectTracking;
  if (!needsObjectTracking && !needsShadowSemanticTracking &&
      needsBatchTracking && !needsProbe && tag != War3BatchTag::Unknown &&
      !keepBridgeForConservativeMerge) {
    BatchTagStageScopeLite scope;
    scope.begin(tag, elementStage);
    const int res = g_trampolineDispatchCommon(thisPtr, edx, a3, a4, a5);
    publishVisibleAfterDispatch();
    scope.end();
    War3RenderState::SetTlsDispatchHandle(0);
    return res;
  }

  dxvk::war3::render::ExecBatchContext ctx = {};
  {
    auto beginScope = MakeRenderHookCpuScope("Hook_Dispatch_Common/BridgeBegin");
    ctx = dxvk::war3::render::ExecBatchProcessor::Begin(edx, tag, elementStage,
                                                        false);
  }
  int res = 0;
  {
    auto origScope = MakeRenderHookCpuScope("Hook_Dispatch_Common/Orig");
    res = g_trampolineDispatchCommon(thisPtr, edx, a3, a4, a5);
  }
  {
    auto publishScope =
        MakeRenderHookCpuScope("Hook_Dispatch_Common/PublishVisible");
    publishVisibleAfterDispatch();
  }
  {
    auto endScope = MakeRenderHookCpuScope("Hook_Dispatch_Common/BridgeEnd");
    dxvk::war3::render::ExecBatchProcessor::End(ctx);
  }
  War3RenderState::SetTlsDispatchHandle(0);
  return res;
}

int __fastcall Hook_RenderQueue_Dispatch_Special(void *thisPtr, void *edx,
                                                 void *a3, void *a4) {
  // Special Dispatch 与 Common Dispatch 对齐同一套快速路径与桥接策略。
  auto perfScope = MakeRenderHookCpuScope("Hook_Dispatch_Special");
  if constexpr (dxvk::war3::internal::kShadowSemanticDispatchContractProbeEnabled) {
    dxvk::war3::render::VisibleRenderableRecord visible = {};
    dxvk::war3::reimpl::RenderBatchElement syntheticBatch = {};
    syntheticBatch.renderablePart = edx;
    syntheticBatch.layerIndex = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a3));
    if (dxvk::war3::render::VisibleRenderableRegistry::instance()
            .queryByRenderablePart(edx, visible)) {
      syntheticBatch.layerStatePtr = visible.layerState;
      syntheticBatch.subIndex = visible.subIndex;
      dxvk::war3::reimpl::MaybeLogSemanticDispatchContract(
          &syntheticBatch, thisPtr != nullptr ? thisPtr : visible.sceneNode,
          visible.meshData);
    } else {
      dxvk::war3::reimpl::LogSemanticDispatchSkip(
          "dispatch-registry-miss", &syntheticBatch, thisPtr, nullptr, nullptr,
          dxvk::war3::render::kInvalidVisibleMeshIndex, 0u);
    }
  }

  War3BatchTag tag = War3RenderState::GetTlsBatchTag();
  int elementStage = -1;
  const bool needsObjectTracking = War3RenderState::NeedsObjectTracking();
  const bool needsShadowObjectIdentity =
      War3RenderState::NeedsShadowObjectIdentity();
  const bool needsShadowFallbackBridge =
      War3RenderState::NeedsShadowDrawFallbackBridge();
  const bool needsShadowSemanticTracking =
      War3RenderState::NeedsShadowSemanticTracking();
  const bool needsBatchTracking = War3RenderState::IsBatchTagTrackingEnabled();
  const bool needsProbe = dxvk::war3::render::NativeRendererProbe::IsEnabled();

  auto publishVisibleAfterDispatch = [&]() {
    if (!needsShadowSemanticTracking ||
        !dxvk::war3::hooks::IsWorldBridgeTag(tag))
      return;
    const uint32_t layerIndex =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a3));
    PublishVisibleRenderableFromDispatch(thisPtr, edx, layerIndex, nullptr);
  };

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeWhenNoTracking &&
      !needsObjectTracking && !needsShadowSemanticTracking &&
      !needsBatchTracking && !needsProbe) {
    const int res = g_trampolineDispatchSpecial(thisPtr, edx, a3, a4);
    publishVisibleAfterDispatch();
    War3RenderState::SetTlsDispatchHandle(0);
    War3RenderState::SetTlsBatchHandle(0);
    return res;
  }

  const bool canUseWorldGroupTagFastPath =
      needsShadowSemanticTracking && !needsObjectTracking &&
      !needsShadowObjectIdentity && !needsShadowFallbackBridge && !needsProbe &&
      dxvk::war3::hooks::IsWorldBridgeTag(tag);

  if ((!dxvk::war3::internal::kNativeHookFastPathEnabled ||
       needsObjectTracking || needsShadowSemanticTracking ||
       needsBatchTracking || needsProbe) &&
      !canUseWorldGroupTagFastPath) {
    auto &tracker = dxvk::war3::render::RenderQueueTracker::instance();
    tracker.GetTagStage(edx, tag, elementStage);
  }

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeForNonWorldTag &&
      (needsObjectTracking || needsShadowSemanticTracking) &&
      !needsBatchTracking && !needsProbe &&
      tag != War3BatchTag::Unknown &&
      !dxvk::war3::hooks::IsWorldBridgeTag(tag)) {
    const int res = g_trampolineDispatchSpecial(thisPtr, edx, a3, a4);
    publishVisibleAfterDispatch();
    War3RenderState::SetTlsDispatchHandle(0);
    War3RenderState::SetTlsBatchHandle(0);
    return res;
  }

  const bool keepBridgeForConservativeMerge =
      dxvk::war3::internal::kNativeConservativeMergedSubmitEnabled &&
      dxvk::war3::internal::
          kNativeConservativeMergedSubmitAllowNoObjectTracking;
  if (!needsObjectTracking && !needsShadowSemanticTracking &&
      needsBatchTracking && !needsProbe && tag != War3BatchTag::Unknown &&
      !keepBridgeForConservativeMerge) {
    BatchTagStageScopeLite scope;
    scope.begin(tag, elementStage);
    const int res = g_trampolineDispatchSpecial(thisPtr, edx, a3, a4);
    publishVisibleAfterDispatch();
    scope.end();
    War3RenderState::SetTlsDispatchHandle(0);
    return res;
  }

  dxvk::war3::render::ExecBatchContext ctx = {};
  {
    auto beginScope = MakeRenderHookCpuScope("Hook_Dispatch_Special/BridgeBegin");
    ctx = dxvk::war3::render::ExecBatchProcessor::Begin(edx, tag, elementStage,
                                                        true);
  }
  int res = 0;
  {
    auto origScope = MakeRenderHookCpuScope("Hook_Dispatch_Special/Orig");
    res = g_trampolineDispatchSpecial(thisPtr, edx, a3, a4);
  }
  {
    auto publishScope =
        MakeRenderHookCpuScope("Hook_Dispatch_Special/PublishVisible");
    publishVisibleAfterDispatch();
  }
  {
    auto endScope = MakeRenderHookCpuScope("Hook_Dispatch_Special/BridgeEnd");
    dxvk::war3::render::ExecBatchProcessor::End(ctx);
  }
  War3RenderState::SetTlsDispatchHandle(0);
  return res;
}

static void Reimpl_GetTrackerTagStage(void *renderablePart, War3BatchTag &tag,
                                      int &stage) {
  // reimpl 队列需要通过统一回调读取 tracker 的 tag/stage 元数据。
  auto &tracker = dxvk::war3::render::RenderQueueTracker::instance();
  tracker.GetTagStage(renderablePart, tag, stage);
}

static void Reimpl_ExecBeginValue(void *element, War3BatchTag tag,
                                  int elementStage, bool isType3,
                                  void *outCtx) {
  // 将 ExecBatch Begin 的上下文写入外部缓冲，供 reimpl 路径在 End 时回放。
  if (!outCtx)
    return;
  auto *ctx = reinterpret_cast<dxvk::war3::render::ExecBatchContext *>(outCtx);
  *ctx = dxvk::war3::render::ExecBatchProcessor::Begin(element, tag,
                                                       elementStage, isType3);
}

static void Reimpl_ExecEndValue(void *ctx) {
  // 对应 Begin 的收口，确保状态机在 reimpl 路径也能正确恢复。
  if (!ctx)
    return;
  auto *typed = reinterpret_cast<dxvk::war3::render::ExecBatchContext *>(ctx);
  dxvk::war3::render::ExecBatchProcessor::End(*typed);
}

static int CallOriginalFlushSortedItems() {
  // 优先调用 trampoline，保持 Hook 链一致；original 作为兜底。
  if (g_trampolineFlushSortedItems)
    return g_trampolineFlushSortedItems();
  if (g_originalFlushSortedItems)
    return g_originalFlushSortedItems();
  return 0;
}

int __cdecl Hook_FlushSortedItems() {
  // FlushSortedItems 支持可选“队列接管模式”：
  // - 关闭接管：调用原函数；
  // - 开启接管：走 reimpl RenderQueue 路径。
  auto perfScope = MakeRenderHookCpuScope("Hook_FlushSortedItems");
  auto maybeLogTakeoverGate = [&](const char *phase,
                                  const War3QueueTakeoverDecision *decision,
                                  D3D9DeviceEx *deviceForLog,
                                  DispatchCommonFn dispatchCommonForLog,
                                  DispatchSpecialFn dispatchSpecialForLog) {
    if (!war3dbg::RenderLogEnabled())
      return;
    static std::atomic<uint32_t> s_gateLogCount{0};
    const uint32_t logIndex =
        s_gateLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (!(logIndex < 64u || (logIndex % 2048u) == 0u))
      return;

    const uint32_t opaqueCount = g_numOfElementsPtr ? *g_numOfElementsPtr : 0u;
    const uint32_t transparentCount =
        g_numOfTransparentPtr ? *g_numOfTransparentPtr : 0u;
    const uint32_t mode =
        decision != nullptr ? uint32_t(decision->mode)
                            : uint32_t(War3QueueTakeoverMode::Fallback);
    const uint32_t reason =
        decision != nullptr ? uint32_t(decision->reason)
                            : uint32_t(War3QueueTakeoverReason::Unknown);
    war3dbg::Print(
        "DXVK SemanticDispatchGate: phase=%s mode=%u reason=%u opaque=%u "
        "transparent=%u device=%p dispatch=%p/%p batchPtr=%p sortedPtr=%p\n",
        phase, mode, reason, opaqueCount, transparentCount, deviceForLog,
        reinterpret_cast<void *>(dispatchCommonForLog),
        reinterpret_cast<void *>(dispatchSpecialForLog),
        g_batchArrayPtr ? *g_batchArrayPtr : nullptr,
        g_sortedBatchPtrs ? reinterpret_cast<void *>(g_sortedBatchPtrs) : nullptr);
  };

  auto *device = dxvk::war3::GetActiveDevice();
  if (!device || !g_numOfElementsPtr || !g_batchArrayPtr ||
      !g_sortedBatchCountPtr || !g_sortedBatchPtrs) {
    maybeLogTakeoverGate("missing-globals", nullptr, device, nullptr, nullptr);
    return CallOriginalFlushSortedItems();
  }

  dxvk::war3::reimpl::RenderQueueGlobals globals = {};
  // 将原生全局队列指针桥接给 reimpl，避免复制大型队列数据。
  globals.numOfElementsPtr = g_numOfElementsPtr;
  globals.batchArrayPtr = g_batchArrayPtr;
  globals.aucTransparentCountPtr = g_numOfTransparentPtr;
  globals.aucTransparentArrayBase = g_transparentArrayBasePtr;
  globals.aucTransparentSortedPtrs = g_transparentSortedPtrs;
  globals.sortedBatchCountPtr = g_sortedBatchCountPtr;
  globals.sortedBatchPtrs = g_sortedBatchPtrs;
  globals.stateOptEnabledPtr = g_stateOptEnabledPtr;
  globals.stateCleanupPendingPtr = g_stateCleanupPendingPtr;

  dxvk::war3::reimpl::RenderQueueFns fns = {};
  // reimpl 所需回调：状态块应用、阶段更新、执行桥接。
  fns.applyStateBlock = g_renderQueueApplyStateBlock;
  fns.stageUpdate = g_renderQueueStageUpdate;
  fns.gxCleanup74 = g_renderQueueGxCleanup74;
  fns.gxCleanup78 = g_renderQueueGxCleanup78;
  fns.getTrackerTagStage = &Reimpl_GetTrackerTagStage;
  fns.execBeginValue = &Reimpl_ExecBeginValue;
  fns.execEndValue = &Reimpl_ExecEndValue;
  fns.sub_13A0E0 = g_transparentDispatchType0;
  fns.sub_198C00 = g_transparentDispatchType1;
  fns.sub_19DFF0 = g_transparentDispatchType2;
  fns.sub_19BC20 = g_transparentDispatchType3;
  fns.sub_13A0B0 = g_transparentDispatchType4;

  auto dispatchCommon =
      g_trampolineDispatchCommon
          ? g_trampolineDispatchCommon
          : reinterpret_cast<DispatchCommonFn>(g_originalExecBatch0);
  auto dispatchSpecial =
      g_trampolineDispatchSpecial
          ? g_trampolineDispatchSpecial
          : reinterpret_cast<DispatchSpecialFn>(g_originalExecBatch3);

  // 若关键 dispatch 缺失则回退原生路径，保证稳定性优先。
  if (!dispatchCommon || !dispatchSpecial) {
    maybeLogTakeoverGate("missing-dispatch", nullptr, device, dispatchCommon,
                         dispatchSpecial);
    return CallOriginalFlushSortedItems();
  }

  War3QueueTakeoverContext takeoverCtx = {};
  takeoverCtx.opaqueCountPtr = g_numOfElementsPtr;
  takeoverCtx.transparentCountPtr = g_numOfTransparentPtr;
  takeoverCtx.transparentArrayBasePtr = g_transparentArrayBasePtr;
  takeoverCtx.transparentSortedPtrs = g_transparentSortedPtrs;
  takeoverCtx.transparentDispatchType0 =
      reinterpret_cast<const void *>(g_transparentDispatchType0);
  takeoverCtx.transparentDispatchType1 =
      reinterpret_cast<const void *>(g_transparentDispatchType1);
  takeoverCtx.transparentDispatchType2 =
      reinterpret_cast<const void *>(g_transparentDispatchType2);
  takeoverCtx.transparentDispatchType3 =
      reinterpret_cast<const void *>(g_transparentDispatchType3);
  takeoverCtx.transparentDispatchType4 =
      reinterpret_cast<const void *>(g_transparentDispatchType4);
  takeoverCtx.hasOriginalFlushTransparent = g_originalFlushTransparent != nullptr;

  const War3QueueTakeoverDecision decision =
      EvaluateQueueTakeoverDecision(takeoverCtx);
  maybeLogTakeoverGate(
      decision.mode == War3QueueTakeoverMode::Fallback ? "fallback" : "takeover",
      &decision, device, dispatchCommon, dispatchSpecial);
  if (decision.mode == War3QueueTakeoverMode::Fallback) {
    return CallOriginalFlushSortedItems();
  }

  // 执行 opaque + transparent 两段 flush，语义与原生队列流程保持一致。
  bool ok = dxvk::war3::reimpl::RenderQueue::FlushSortedItems_StdSort(
      device, globals, g_renderQueueItemComparator, dispatchCommon,
      dispatchSpecial, fns);
  if (!ok) {
    maybeLogTakeoverGate("opaque-failed", &decision, device, dispatchCommon,
                         dispatchSpecial);
    return CallOriginalFlushSortedItems();
  }

  const uint32_t transparentCount =
      g_numOfTransparentPtr ? *g_numOfTransparentPtr : 0u;
  if (transparentCount > 0u) {
    bool transparentOk = false;
    if (decision.mode == War3QueueTakeoverMode::Full &&
        dxvk::war3::internal::kNativeQueueTakeoverUseNativeTransparentFlush &&
        g_originalFlushTransparent) {
      g_originalFlushTransparent();
      transparentOk = true;
    } else {
      transparentOk = dxvk::war3::reimpl::RenderQueue::FlushTransparent_StdSort(
          globals, g_renderQueueItemComparator, fns);
      if (!transparentOk && g_originalFlushTransparent) {
        g_originalFlushTransparent();
        transparentOk = true;
      }
    }

    if (!transparentOk) {
      maybeLogTakeoverGate("transparent-failed", &decision, device,
                           dispatchCommon, dispatchSpecial);
      return CallOriginalFlushSortedItems();
    }
  }

  return 0;
}

void __fastcall Hook_Terrain_RenderAllTiles(void *thisPtr, void * /*edx*/) {
  // 地形入口仅负责地形层标记，确保后续分类逻辑能识别 terrain phase。
  auto perfScope = MakeRenderHookCpuScope("Hook_Terrain_RenderAllTiles");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_HOOK_HOTPATH_LOG(
        "DXVK War3Hook: Hook_Terrain_RenderAllTiles FIRST_CALL this=0x%p\n",
        thisPtr);
  }

  War3RenderState::OnTerrainEnter();
  if (g_trampolineTerrainAllTiles) {
    g_trampolineTerrainAllTiles(thisPtr, nullptr);
  } else if (g_originalTerrainAllTiles) {
    g_originalTerrainAllTiles(thisPtr, nullptr);
  }
  War3RenderState::OnTerrainExit();
}

void War3HookRender::Install(uintptr_t gameBase) {
  const auto &book = GetWar3HookAddressBook127a();
  // 安装流程：
  // 1) 解析所有渲染域地址；
  // 2) 缓存原函数指针；
  // 3) 分项安装 Hook。
  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };
  auto resolveData = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  LPVOID renderDispatcherAddr = resolveCode(book.renderDispatcher);
  LPVOID worldFrameUpdateAddr = resolveCode(book.worldFrameUpdateAndPreparePasses);
  LPVOID worldRenderSceneAddr = resolveCode(book.worldRenderScene);
  LPVOID sceneSubmitBatchAddr = resolveCode(book.sceneSubmitBatch);
  LPVOID worldDispatchAddr = resolveCode(book.worldDispatch);
  LPVOID worldObjectsGroupAddr = resolveCode(book.worldObjectsRenderGroup);
  LPVOID dispatchCommonAddr = resolveCode(book.dispatchCommon);
  LPVOID dispatchSpecialAddr = resolveCode(book.dispatchSpecial);
  LPVOID flushSortedAddr = resolveCode(book.flushSortedItems);
  LPVOID flushTransparentAddr = resolveCode(book.rqFlushTransparent);
  LPVOID terrainAllTilesAddr = resolveCode(book.terrainRenderAllTiles);
  LPVOID transparentArrayBaseAddr = resolveData(book.rqTransparentArrayBasePtr);
  LPVOID transparentSortedPtrsAddr = resolveData(book.rqTransparentSortedPtrs);
  LPVOID transparentDispatchType0Addr =
      resolveCode(book.rqTransparentDispatchType0);
  LPVOID transparentDispatchType1Addr =
      resolveCode(book.rqTransparentDispatchType1);
  LPVOID transparentDispatchType2Addr =
      resolveCode(book.rqTransparentDispatchType2);
  LPVOID transparentDispatchType3Addr =
      resolveCode(book.rqTransparentDispatchType3);
  LPVOID transparentDispatchType4Addr =
      resolveCode(book.rqTransparentDispatchType4);

  LPVOID execBatch0Addr = resolveCode(book.dispatchCommon);
  LPVOID execBatch3Addr = resolveCode(book.dispatchSpecial);

  g_originalRenderDispatcher =
      reinterpret_cast<RenderDispatcherFn>(renderDispatcherAddr);
  g_originalWorldFrameUpdate =
      reinterpret_cast<WorldFrameUpdateAndPreparePassesFn>(worldFrameUpdateAddr);
  g_originalWorldRenderScene =
      reinterpret_cast<WorldRenderSceneFn>(worldRenderSceneAddr);
  g_originalSceneSubmitBatch =
      reinterpret_cast<SceneSubmitBatchFn>(sceneSubmitBatchAddr);
  g_originalWorldDispatch =
      reinterpret_cast<WorldDispatchFn>(worldDispatchAddr);
  g_originalWorldObjectsRenderGroup =
      reinterpret_cast<WorldObjectsRenderGroupFn>(worldObjectsGroupAddr);
  g_originalExecBatch0 = reinterpret_cast<DispatchCommonFn>(execBatch0Addr);
  g_originalExecBatch3 = reinterpret_cast<DispatchSpecialFn>(execBatch3Addr);
  g_originalFlushSortedItems =
      reinterpret_cast<FlushSortedItemsFn>(flushSortedAddr);
  g_originalFlushTransparent =
      reinterpret_cast<FlushTransparentFn>(flushTransparentAddr);
  g_originalTerrainAllTiles =
      reinterpret_cast<TerrainRenderAllTilesFn>(terrainAllTilesAddr);
  g_transparentArrayBasePtr =
      reinterpret_cast<void **>(transparentArrayBaseAddr);
  g_transparentSortedPtrs = reinterpret_cast<void **>(transparentSortedPtrsAddr);
  g_transparentDispatchType0 =
      reinterpret_cast<TransparentDispatchType0Fn>(transparentDispatchType0Addr);
  g_transparentDispatchType1 =
      reinterpret_cast<TransparentDispatchTypeXFn>(transparentDispatchType1Addr);
  g_transparentDispatchType2 =
      reinterpret_cast<TransparentDispatchTypeXFn>(transparentDispatchType2Addr);
  g_transparentDispatchType3 =
      reinterpret_cast<TransparentDispatchTypeXFn>(transparentDispatchType3Addr);
  g_transparentDispatchType4 =
      reinterpret_cast<TransparentDispatchTypeXFn>(transparentDispatchType4Addr);

  InstallMinHook(renderDispatcherAddr,
                 reinterpret_cast<LPVOID>(&Hook_RenderDispatcher),
                 reinterpret_cast<LPVOID *>(&g_trampolineRenderDispatcher),
                 "Render", "RenderDispatcher", false, false);
  InstallMinHook(worldFrameUpdateAddr,
                 reinterpret_cast<LPVOID>(&Hook_WorldFrameUpdateAndPreparePasses),
                 reinterpret_cast<LPVOID *>(&g_trampolineWorldFrameUpdate),
                 "Render", "WorldFrameUpdateAndPreparePasses", false, false);
  InstallMinHook(worldRenderSceneAddr,
                 reinterpret_cast<LPVOID>(&Hook_WorldRenderScene),
                 reinterpret_cast<LPVOID *>(&g_trampolineWorldRenderScene),
                 "Render", "WorldRenderScene", false, false);
  InstallMinHook(sceneSubmitBatchAddr,
                 reinterpret_cast<LPVOID>(&Hook_SceneSubmitBatch),
                 reinterpret_cast<LPVOID *>(&g_trampolineSceneSubmitBatch),
                 "Render", "SceneSubmitBatch", false, false);
  InstallMinHook(worldDispatchAddr, reinterpret_cast<LPVOID>(&Hook_WorldDispatch),
                 reinterpret_cast<LPVOID *>(&g_trampolineWorldDispatch),
                 "Render", "WorldDispatch", false, false);
  InstallMinHook(worldObjectsGroupAddr,
                 reinterpret_cast<LPVOID>(&Hook_WorldObjects_RenderGroup),
                 reinterpret_cast<LPVOID *>(&g_trampolineWorldObjectsRenderGroup),
                 "Render", "WorldObjects_RenderGroup", false, false);
  InstallMinHook(dispatchCommonAddr,
                 reinterpret_cast<LPVOID>(&Hook_RenderQueue_Dispatch_Common),
                 reinterpret_cast<LPVOID *>(&g_trampolineDispatchCommon),
                 "Render", "RenderQueue_Dispatch_Common", false, false);
  InstallMinHook(dispatchSpecialAddr,
                 reinterpret_cast<LPVOID>(&Hook_RenderQueue_Dispatch_Special),
                 reinterpret_cast<LPVOID *>(&g_trampolineDispatchSpecial),
                 "Render", "RenderQueue_Dispatch_Special", false, false);
  InstallMinHook(flushSortedAddr, reinterpret_cast<LPVOID>(&Hook_FlushSortedItems),
                 reinterpret_cast<LPVOID *>(&g_trampolineFlushSortedItems),
                 "Render", "FlushSortedItems", false, false);
  InstallMinHook(terrainAllTilesAddr,
                 reinterpret_cast<LPVOID>(&Hook_Terrain_RenderAllTiles),
                 reinterpret_cast<LPVOID *>(&g_trampolineTerrainAllTiles),
                 "Render", "Terrain_RenderAllTiles", false, false);

  War3HookRenderIdentity::Install(gameBase);
}

void *War3HookRender::GetTrampolineDispatchSpecial() {
  return reinterpret_cast<void *>(g_trampolineDispatchSpecial);
}

void *War3HookRender::GetTrampolineDispatchCommon() {
  return reinterpret_cast<void *>(g_trampolineDispatchCommon);
}

void *War3HookRender::GetTrampolineWorldObjectsRenderGroup() {
  return reinterpret_cast<void *>(g_trampolineWorldObjectsRenderGroup);
}

void War3HookRender::ResetDispatchMergeContext() {
  // Old 1cb render hook has no local-merge cache to flush.
}

} // namespace dxvk::war3::hooks
