#include "war3_hook_render.h"
#include "war3_hook_address_book.h"
#include "war3_hook_lifecycle.h"
#include "war3_hook_perf.h"
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
#include "../gpu_skin/war3_gpu_skin_native_bridge.h"
#include "../reimpl/war3_render_queue.h"
#include "../render/war3_native_renderer_probe.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_render_dispatcher.h"
#include "../render/war3_render_exec_batch.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_render_state.h"
#include "../render/war3_current_draw_contract.h"
#include "../render/war3_lightning_runtime.h"
#include "../render/war3_renderer.h"
#include "../render/war3_shadow_lifecycle.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../render/war3_visible_renderables.h"
#include "../platform/war3_native_device_resolver.h"
#include "../platform/war3_runtime_bootstrap.h"
#include "../state/war3_render_state.h"
#include "../tools/war3_perf_monitor.h"
#include "../../util/util_bit.h"
#include "../../../util/log/log.h"
#include "../../../util/util_time.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

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
using ApplyDrawStateAndSamplerPairFn =
    int(__fastcall *)(void *, void *, void *);
using ApplyDrawStateAndDrawFn = int(__fastcall *)(void *, void *, int);
using GxDeviceD3dDynamicVertexUploadFn =
    int(__thiscall *)(void *, uint32_t, const void *, uint32_t, const void *,
                     uint32_t, const void *, uint32_t, const void *, uint32_t,
                     const void *, uint32_t, const void *, uint32_t);
using GxDeviceD3dSkinCopyKernelFn = void(__thiscall *)(void *, void *);
using TerrainRenderAllTilesFn = void(__fastcall *)(void *, void *);
using FlushSortedItemsFn = int(__cdecl *)();
using FlushTransparentFn = int(__cdecl *)();
using TransparentDispatchType0Fn = void(__fastcall *)(uint32_t, void *);
using TransparentDispatchTypeXFn = void(__fastcall *)(void *);
using WorldPrepareCameraBuildFrustumFn =
    int(__fastcall *)(void *, void *, void *);
using WorldPrepareTerrainShadowFlushFn =
    int(__fastcall *)(void *, int);
using WorldPrepareTerrainExtraPassFn =
    int(__fastcall *)(void *, void *, float);
using WorldPrepareShadowProjectorFlushFn = int(__cdecl *)();
using WorldPrepareTimedAdvanceFn = void(__thiscall *)(void *, float);
using WorldPrepareFlushDeferredSelectionObjectsFn =
    int(__thiscall *)(void *);
using WorldPrepareGlobalRenderCallbackPassFn =
    int(__thiscall *)(void *);
using WorldPrepareRenderWaypointIndicatorsFn =
    int(__thiscall *)(void *, float, const uint32_t *);
using WorldPrepareFrameUpdateGateFn =
    int(__thiscall *)(void *, float, float, void *);
using WorldPrepareThisNoArgsFn = int(__thiscall *)(void *);
using WorldPrepareCameraAdvanceFn =
    int(__thiscall *)(void *, float, void *);
using WorldPrepareSceneQueryFlushSyncFn = int(__cdecl *)();
using WorldPrepareFixedPointRemapFn =
    int(__fastcall *)(void *, void *, const void *);
using WorldPreparePostVisibilityFrameAnchorUpdateFn =
    void(__thiscall *)(void *);
using WorldPreparePostVisibilityFrameAnchorVisibilityQueryFn =
    int(__fastcall *)(void *, void *, void *);
using WorldPrepareVisibilityTailAdvanceFn =
    int(__thiscall *)(void *, int, int, float);

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

static ApplyDrawStateAndSamplerPairFn
    g_trampolineApplyDrawStateAndSamplerPair = nullptr;

static ApplyDrawStateAndDrawFn g_originalApplyDrawStateAndDraw = nullptr;
static ApplyDrawStateAndDrawFn g_trampolineApplyDrawStateAndDraw = nullptr;

static GxDeviceD3dDynamicVertexUploadFn
    g_trampolineGxDeviceD3dDynamicVertexUpload = nullptr;
static GxDeviceD3dSkinCopyKernelFn
    g_trampolineGxDeviceD3dSkinCopyKernel = nullptr;

static TerrainRenderAllTilesFn g_originalTerrainAllTiles = nullptr;
static TerrainRenderAllTilesFn g_trampolineTerrainAllTiles = nullptr;

static FlushSortedItemsFn g_originalFlushSortedItems = nullptr;
static FlushSortedItemsFn g_trampolineFlushSortedItems = nullptr;
static FlushTransparentFn g_originalFlushTransparent = nullptr;
static FlushTransparentFn g_trampolineFlushTransparentTiming = nullptr;
static dxvk::war3::reimpl::StageUpdateFn g_trampolineStageUpdateTiming =
    nullptr;

static void **g_transparentArrayBasePtr = nullptr;
static void **g_transparentSortedPtrs = nullptr;
static TransparentDispatchType0Fn g_transparentDispatchType0 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType1 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType2 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType3 = nullptr;
static TransparentDispatchTypeXFn g_transparentDispatchType4 = nullptr;
static TransparentDispatchType0Fn g_trampolineTransparentDispatchType0 =
    nullptr;
static TransparentDispatchTypeXFn g_trampolineTransparentDispatchType1 =
    nullptr;
static TransparentDispatchTypeXFn g_trampolineTransparentDispatchType2 =
    nullptr;
static TransparentDispatchTypeXFn g_trampolineTransparentDispatchType3 =
    nullptr;
static TransparentDispatchTypeXFn g_trampolineTransparentDispatchType4 =
    nullptr;

static WorldPrepareCameraBuildFrustumFn
    g_trampolineWorldPrepareCameraBuildFrustum = nullptr;
static WorldPrepareTerrainShadowFlushFn
    g_trampolineWorldPrepareTerrainShadowFlush = nullptr;
static WorldPrepareTerrainExtraPassFn
    g_trampolineWorldPrepareTerrainExtraPass = nullptr;
static WorldPrepareShadowProjectorFlushFn
    g_trampolineWorldPrepareShadowProjectorFlush = nullptr;
static WorldPrepareTimedAdvanceFn
    g_trampolineWorldPrepareTargetIndicatorRingAdvance = nullptr;
static WorldPrepareTimedAdvanceFn
    g_trampolineWorldPrepareCinematicFilterTimeAdvance = nullptr;
static WorldPrepareTimedAdvanceFn
    g_trampolineWorldPrepareRuntimeFlagClockAdvance3B8760 = nullptr;
static WorldPrepareFlushDeferredSelectionObjectsFn
    g_trampolineWorldPrepareFlushDeferredSelectionObjects = nullptr;
static WorldPrepareGlobalRenderCallbackPassFn
    g_trampolineWorldPrepareGlobalRenderCallbackPass = nullptr;
static WorldPrepareRenderWaypointIndicatorsFn
    g_trampolineWorldPrepareRenderWaypointIndicators = nullptr;
static WorldPrepareFrameUpdateGateFn
    g_trampolineWorldPrepareFrameUpdateGate = nullptr;
static WorldPrepareThisNoArgsFn
    g_trampolineWorldPrepareGameUiFrameSync = nullptr;
static WorldPrepareThisNoArgsFn
    g_trampolineWorldPrepareUpdateIndicatorAnchor = nullptr;
static WorldPrepareCameraAdvanceFn
    g_trampolineWorldPrepareCameraAdvance = nullptr;
static WorldPrepareThisNoArgsFn
    g_trampolineWorldPrepareCameraPrepareConstants = nullptr;
static WorldPrepareThisNoArgsFn
    g_trampolineWorldPrepareViewProjPrepare = nullptr;
static WorldPrepareSceneQueryFlushSyncFn
    g_trampolineWorldPrepareSceneQueryFlushSync = nullptr;
static WorldPrepareFixedPointRemapFn
    g_trampolineWorldPrepareFixedPointRemap = nullptr;
static WorldPrepareSceneQueryFlushSyncFn
    g_trampolineWorldPreparePostVisibilityGlobalAdvanceA = nullptr;
static WorldPreparePostVisibilityFrameAnchorUpdateFn
    g_trampolineWorldPreparePostVisibilityFrameAnchorUpdate = nullptr;
static WorldPreparePostVisibilityFrameAnchorVisibilityQueryFn
    g_trampolineWorldPreparePostVisibilityFrameAnchorVisibilityQuery = nullptr;
static WorldPrepareSceneQueryFlushSyncFn
    g_trampolineWorldPreparePostVisibilityGlobalAdvanceB = nullptr;
static WorldPrepareVisibilityTailAdvanceFn
    g_trampolineWorldPrepareVisibilityTailAdvanceA = nullptr;
static WorldPrepareVisibilityTailAdvanceFn
    g_trampolineWorldPrepareVisibilityTailAdvanceB = nullptr;

#define WAR3_HOOK_HOTPATH_LOG(fmt, ...)                                        \
  do {                                                                         \
    if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {    \
      WAR3_RENDER_LOG(fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

struct NativeGpuSkinOwnedHook {
  const char* name = nullptr;
  LPVOID target = nullptr;
  LPVOID detour = nullptr;
  LPVOID trampoline = nullptr;
  bool created = false;
  bool enabled = false;
};

static NativeGpuSkinOwnedHook g_gpuSkinApplyHook;
static NativeGpuSkinOwnedHook g_gpuSkinUploadHook;
static NativeGpuSkinOwnedHook g_gpuSkinKernelHook;

enum class NativeGpuSkinDetourKind : uint8_t {
  Apply = 0,
  OuterUpload,
  Kernel,
  Flush,
};

enum class NativeGpuSkinDetourState : uint32_t {
  Disabled = 0,
  Installing,
  Running,
  Draining,
  SafetyOnly,
};

struct NativeGpuSkinDetourActivity {
  std::atomic<uint32_t> apply{0u};
  std::atomic<uint32_t> outerUpload{0u};
  std::atomic<uint32_t> kernel{0u};
  std::atomic<uint32_t> flush{0u};
  std::atomic<bool> admitFlushPins{false};
  std::atomic<NativeGpuSkinDetourState> state{
      NativeGpuSkinDetourState::Disabled};
};

static NativeGpuSkinDetourActivity g_gpuSkinDetourActivity;

// IDA ASM binds the only direct skin-kernel call to 0x0EEB85 inside the
// outer-upload function. Keep a live dynamic proof as well: a nested kernel
// can borrow the already-published outer detour pin, while any unexpected
// standalone call retains its own process-wide counter. The outer destructor
// clears TLS before dropping the global pin, so hook removal cannot observe a
// false quiet interval.
static thread_local uint32_t t_nativeGpuSkinOuterDetourDepth = 0u;
static thread_local bool t_nativeGpuSkinOuterDetourDepthFaulted = false;

static std::atomic<uint32_t>& NativeGpuSkinDetourCounter(
    NativeGpuSkinDetourKind kind) noexcept {
  switch (kind) {
    case NativeGpuSkinDetourKind::Apply:
      return g_gpuSkinDetourActivity.apply;
    case NativeGpuSkinDetourKind::OuterUpload:
      return g_gpuSkinDetourActivity.outerUpload;
    case NativeGpuSkinDetourKind::Kernel:
      return g_gpuSkinDetourActivity.kernel;
    case NativeGpuSkinDetourKind::Flush:
      return g_gpuSkinDetourActivity.flush;
  }
  return g_gpuSkinDetourActivity.kernel;
}

class NativeGpuSkinDetourPin {
public:
  explicit NativeGpuSkinDetourPin(
      NativeGpuSkinDetourKind kind) noexcept
      : m_kind(kind) {
    if (kind == NativeGpuSkinDetourKind::Kernel &&
        t_nativeGpuSkinOuterDetourDepth != 0u &&
        !t_nativeGpuSkinOuterDetourDepthFaulted) {
      return;
    }
    if (kind == NativeGpuSkinDetourKind::Flush &&
        !g_gpuSkinDetourActivity.admitFlushPins.load(
            std::memory_order_acquire)) {
      return;
    }
    auto& counter = NativeGpuSkinDetourCounter(kind);
    counter.fetch_add(1u, std::memory_order_acq_rel);
    if (kind == NativeGpuSkinDetourKind::Flush &&
        !g_gpuSkinDetourActivity.admitFlushPins.load(
            std::memory_order_acquire)) {
      counter.fetch_sub(1u, std::memory_order_acq_rel);
      return;
    }
    m_pinned = true;
    if (kind == NativeGpuSkinDetourKind::OuterUpload) {
      if (t_nativeGpuSkinOuterDetourDepth ==
          std::numeric_limits<uint32_t>::max()) {
        t_nativeGpuSkinOuterDetourDepthFaulted = true;
      } else {
        ++t_nativeGpuSkinOuterDetourDepth;
        m_outerDepthPublished = true;
      }
    }
  }

  ~NativeGpuSkinDetourPin() noexcept {
    if (m_outerDepthPublished) {
      if (t_nativeGpuSkinOuterDetourDepth == 0u) {
        t_nativeGpuSkinOuterDetourDepthFaulted = true;
      } else {
        --t_nativeGpuSkinOuterDetourDepth;
      }
    }
    if (m_pinned) {
      NativeGpuSkinDetourCounter(m_kind).fetch_sub(
          1u, std::memory_order_acq_rel);
    }
  }

  NativeGpuSkinDetourPin(const NativeGpuSkinDetourPin&) = delete;
  NativeGpuSkinDetourPin& operator=(const NativeGpuSkinDetourPin&) = delete;

private:
  NativeGpuSkinDetourKind m_kind;
  bool m_pinned = false;
  bool m_outerDepthPublished = false;
};

struct NativeProductionTimingTlsState {
  // Only monotonic cohort ordinals live in TLS. Active samples and elapsed
  // stages stay on each hook stack, so nesting or native SEH cannot leak a
  // sampled/active flag into the next call.
  uint64_t outerOrdinal = 0u;
  uint64_t kernelOrdinal = 0u;
  uint64_t dispatchSemanticOrdinal = 0u;
  uint64_t semanticOrdinal = 0u;
};

static thread_local NativeProductionTimingTlsState
    t_nativeProductionTiming;

static_assert(
    (dxvk::war3::gpu_skin::kNativeProductionTimingSamplePeriod &
     (dxvk::war3::gpu_skin::kNativeProductionTimingSamplePeriod - 1u)) == 0u,
    "production timing event ordinals require a power-of-two period");

static bool SampleNextNativeProductionOrdinal(
    uint64_t& ordinal) noexcept {
  if (!dxvk::war3::gpu_skin::
          NativeBridgeProductionLightTimingEnabled()) {
    return false;
  }
  ++ordinal;
  return (ordinal &
          (dxvk::war3::gpu_skin::kNativeProductionTimingSamplePeriod - 1u)) ==
      dxvk::war3::gpu_skin::kNativeProductionTimingSamplePhase;
}

static uint64_t NativeProductionElapsedTicks(
    int64_t start, int64_t end) noexcept {
  return end > start ? uint64_t(end - start) : 0u;
}

class NativeProductionOuterTimingScope {
public:
  NativeProductionOuterTimingScope() noexcept
      : m_sampled(SampleNextNativeProductionOrdinal(
            t_nativeProductionTiming.outerOrdinal)),
        m_inclusiveStart(m_sampled
            ? dxvk::high_resolution_clock::get_counter() : 0) {
  }

  ~NativeProductionOuterTimingScope() noexcept {
    if (!m_sampled)
      return;
    // Capture both inclusive endpoints before the single logical-sample
    // writer begins. Substage atomic publication must not contaminate either
    // the accepted-fast or admission-rejected root measurement.
    const int64_t inclusiveEnd =
        (m_fastAccepted || m_dispatchSealAccepted || m_fallbackRejected)
        ? dxvk::high_resolution_clock::get_counter() : 0;
    // Capacity covers every outer timing enum even though accepted-fast and
    // rejected-fallback are mutually exclusive for one physical call.
    std::array<dxvk::war3::gpu_skin::NativeProductionTimingEntry, 12u>
        entries{};
    size_t count = 0u;
    const auto append = [&](
        dxvk::war3::gpu_skin::NativeProductionTimingStage stage,
        uint64_t ticks, uint8_t bucket) {
      if (count < entries.size())
        entries[count++] = {stage, ticks, bucket};
    };
    const auto appendRoot = [&append](
        dxvk::war3::gpu_skin::NativeProductionTimingStage stage,
        uint64_t ticks) {
      append(stage, ticks, 0u);
    };
    if (m_admissionRecorded) {
      appendRoot(
          m_fastAccepted
              ? dxvk::war3::gpu_skin::
                  NativeProductionTimingStage::OuterAdmissionAccepted
              : dxvk::war3::gpu_skin::
                  NativeProductionTimingStage::OuterAdmissionRejected,
          m_admissionTicks);
      const size_t acceptedClass =
          static_cast<size_t>(m_admissionProbe.acceptedClass);
      if (m_fastAccepted && acceptedClass <
              dxvk::war3::gpu_skin::
                  kNativeOutsideUploadAdmissionClassCount) {
        append(
            dxvk::war3::gpu_skin::
                NativeProductionTimingStage::OuterAdmissionAcceptedClass,
            m_admissionTicks, uint8_t(acceptedClass));
      }
    }
    if (m_fastAccepted) {
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterFastInclusive,
          NativeProductionElapsedTicks(m_inclusiveStart, inclusiveEnd));
    }
    if (m_dispatchSealAccepted) {
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterDispatchSealAdmission,
          m_dispatchSealAdmissionTicks);
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterDispatchSealInclusive,
          NativeProductionElapsedTicks(m_inclusiveStart, inclusiveEnd));
    }
    if (m_bodyRecorded) {
      appendRoot(
          m_dispatchSealAccepted
              ? dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterDispatchSealBody
              : dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterFastBody,
          m_bodyTicks);
    }
    if (m_completeRecorded) {
      appendRoot(
          m_dispatchSealAccepted
              ? dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterDispatchSealComplete
              : dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterFastComplete,
          m_completeTicks);
      const size_t acceptedClass =
          static_cast<size_t>(m_admissionProbe.acceptedClass);
      if (!m_dispatchSealAccepted && acceptedClass <
              dxvk::war3::gpu_skin::
              kNativeOutsideUploadAdmissionClassCount) {
        append(
            dxvk::war3::gpu_skin::
                NativeProductionTimingStage::OuterFastCompleteClass,
            m_completeTicks, uint8_t(acceptedClass));
      }
    }
    if (m_cancelRecorded) {
      appendRoot(
          m_dispatchSealAccepted
              ? dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterDispatchSealCancel
              : dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterFastCancel,
          m_cancelTicks);
    }
    if (m_fallbackRejected) {
      const uint64_t fallbackInclusiveTicks =
          NativeProductionElapsedTicks(
              m_fallbackInclusiveStart, inclusiveEnd);
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterFallbackInclusive,
          fallbackInclusiveTicks);
      const size_t rejectReason =
          static_cast<size_t>(m_admissionProbe.rejectReason);
      if (rejectReason != static_cast<size_t>(
              dxvk::war3::gpu_skin::
                  NativeOutsideUploadRejectReason::Unknown) &&
          rejectReason < dxvk::war3::gpu_skin::
              kNativeOutsideUploadRejectReasonCount) {
        append(
            dxvk::war3::gpu_skin::
                NativeProductionTimingStage::OuterFallbackReason,
            fallbackInclusiveTicks, uint8_t(rejectReason));
      }
    }
    if (m_fallbackBeginRecorded) {
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterFallbackBegin,
          m_fallbackBeginTicks);
    }
    if (m_fallbackBodyRecorded) {
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterFallbackBody,
          m_fallbackBodyTicks);
    }
    if (m_fallbackCompleteRecorded) {
      appendRoot(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterFallbackComplete,
          m_fallbackCompleteTicks);
    }
    dxvk::war3::gpu_skin::RecordNativeProductionTimingBatch(
        entries.data(), count);
  }

  NativeProductionOuterTimingScope(
      const NativeProductionOuterTimingScope&) = delete;
  NativeProductionOuterTimingScope& operator=(
      const NativeProductionOuterTimingScope&) = delete;

  int64_t BeginStage() const noexcept {
    return m_sampled
        ? dxvk::high_resolution_clock::get_counter() : 0;
  }

  dxvk::war3::gpu_skin::NativeOutsideUploadAdmissionProbe*
  AdmissionProbe() noexcept {
    return m_sampled ? &m_admissionProbe : nullptr;
  }

  void EndAdmission(int64_t start, bool accepted) noexcept {
    if (!m_sampled)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    m_fastAccepted = accepted;
    m_fallbackRejected = !accepted;
    m_admissionRecorded = true;
    m_admissionTicks = NativeProductionElapsedTicks(start, end);
    if (!accepted) {
      // Do not charge the sampled admission bookkeeping itself to the
      // production fallback root that will be extrapolated by 256.
      m_fallbackInclusiveStart =
          dxvk::high_resolution_clock::get_counter();
    }
  }

  void EndDispatchSealAdmission(int64_t start) noexcept {
    if (!m_sampled)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    m_dispatchSealAccepted = true;
    m_dispatchSealAdmissionTicks =
        NativeProductionElapsedTicks(start, end);
  }

  void EndStage(
      dxvk::war3::gpu_skin::NativeProductionTimingStage stage,
      int64_t start) noexcept {
    if (!m_sampled)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    const uint64_t ticks = NativeProductionElapsedTicks(start, end);
    switch (stage) {
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFastBody:
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterDispatchSealBody:
        m_bodyRecorded = true;
        m_bodyTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFastComplete:
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterDispatchSealComplete:
        m_completeRecorded = true;
        m_completeTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFastCancel:
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterDispatchSealCancel:
        m_cancelRecorded = true;
        m_cancelTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFallbackBegin:
        m_fallbackBeginRecorded = true;
        m_fallbackBeginTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFallbackBody:
        m_fallbackBodyRecorded = true;
        m_fallbackBodyTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFallbackComplete:
        m_fallbackCompleteRecorded = true;
        m_fallbackCompleteTicks = ticks;
        break;
      default:
        break;
    }
  }

private:
  bool m_sampled = false;
  bool m_fastAccepted = false;
  bool m_dispatchSealAccepted = false;
  bool m_fallbackRejected = false;
  bool m_admissionRecorded = false;
  bool m_bodyRecorded = false;
  bool m_completeRecorded = false;
  bool m_cancelRecorded = false;
  bool m_fallbackBeginRecorded = false;
  bool m_fallbackBodyRecorded = false;
  bool m_fallbackCompleteRecorded = false;
  int64_t m_inclusiveStart = 0;
  int64_t m_fallbackInclusiveStart = 0;
  uint64_t m_admissionTicks = 0u;
  uint64_t m_dispatchSealAdmissionTicks = 0u;
  uint64_t m_bodyTicks = 0u;
  uint64_t m_completeTicks = 0u;
  uint64_t m_cancelTicks = 0u;
  uint64_t m_fallbackBeginTicks = 0u;
  uint64_t m_fallbackBodyTicks = 0u;
  uint64_t m_fallbackCompleteTicks = 0u;
  dxvk::war3::gpu_skin::NativeOutsideUploadAdmissionProbe
      m_admissionProbe{};
};

class NativeProductionKernelTimingScope {
public:
  NativeProductionKernelTimingScope() noexcept {
    if (!dxvk::war3::gpu_skin::
            NativeBridgeProductionLightTimingEnabled()) {
      return;
    }
    const uint64_t ordinal =
        ++t_nativeProductionTiming.kernelOrdinal;
    m_sampled = ordinal %
            dxvk::war3::gpu_skin::kNativeProductionTimingSamplePeriod ==
        dxvk::war3::gpu_skin::kNativeProductionTimingSamplePhase;
    if (m_sampled)
      m_inclusiveStart = dxvk::high_resolution_clock::get_counter();
  }

  ~NativeProductionKernelTimingScope() noexcept {
    if (m_sampled) {
      const int64_t inclusiveEnd =
          dxvk::high_resolution_clock::get_counter();
      std::array<dxvk::war3::gpu_skin::NativeProductionTimingEntry, 4u>
          entries{};
      size_t count = 0u;
      entries[count++] = {
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::KernelInclusive,
          NativeProductionElapsedTicks(m_inclusiveStart, inclusiveEnd)};
      if (m_evaluateRecorded) {
        entries[count++] = {
            dxvk::war3::gpu_skin::
                NativeProductionTimingStage::KernelEvaluate,
            m_evaluateTicks};
      }
      if (m_originalRecorded) {
        entries[count++] = {
            dxvk::war3::gpu_skin::
                NativeProductionTimingStage::KernelOriginal,
            m_originalTicks};
      }
      if (m_notifyRecorded) {
        entries[count++] = {
            dxvk::war3::gpu_skin::
                NativeProductionTimingStage::KernelNotify,
            m_notifyTicks};
      }
      dxvk::war3::gpu_skin::RecordNativeProductionTimingBatch(
          entries.data(), count);
    }
  }

  NativeProductionKernelTimingScope(
      const NativeProductionKernelTimingScope&) = delete;
  NativeProductionKernelTimingScope& operator=(
      const NativeProductionKernelTimingScope&) = delete;

  int64_t BeginStage() const noexcept {
    return m_sampled
        ? dxvk::high_resolution_clock::get_counter() : 0;
  }

  void EndStage(
      dxvk::war3::gpu_skin::NativeProductionTimingStage stage,
      int64_t start) noexcept {
    if (!m_sampled)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    const uint64_t ticks = NativeProductionElapsedTicks(start, end);
    switch (stage) {
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::KernelEvaluate:
        m_evaluateRecorded = true;
        m_evaluateTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::KernelOriginal:
        m_originalRecorded = true;
        m_originalTicks = ticks;
        break;
      case dxvk::war3::gpu_skin::
          NativeProductionTimingStage::KernelNotify:
        m_notifyRecorded = true;
        m_notifyTicks = ticks;
        break;
      default:
        break;
    }
  }

private:
  bool m_sampled = false;
  bool m_evaluateRecorded = false;
  bool m_originalRecorded = false;
  bool m_notifyRecorded = false;
  int64_t m_inclusiveStart = 0;
  uint64_t m_evaluateTicks = 0u;
  uint64_t m_originalTicks = 0u;
  uint64_t m_notifyTicks = 0u;
};

class NativeGpuSkinBridgeFlushScope {
public:
  NativeGpuSkinBridgeFlushScope() noexcept {
    m_eventSequence = dxvk::war3::render::
        CurrentWorldObjectsPhase1PurePeriodicDispatchSequence();
    const int64_t begin = m_eventSequence != 0u
        ? dxvk::high_resolution_clock::get_counter() : 0;
    if (m_eventSequence != 0u) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
    }
    m_active = dxvk::war3::gpu_skin::BeginNativeFlushTransaction();
    if (m_eventSequence != 0u) {
      const int64_t end = dxvk::high_resolution_clock::get_counter();
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
      dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
          m_eventSequence,
          dxvk::war3::render::
              WorldObjectsPhase1PairedTimingStage::FlushTransactionBegin,
          end >= begin ? uint64_t(end - begin) : 0u);
    }
  }

  ~NativeGpuSkinBridgeFlushScope() noexcept {
    const int64_t begin = m_eventSequence != 0u && m_active
        ? dxvk::high_resolution_clock::get_counter() : 0;
    if (m_eventSequence != 0u && m_active) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
    }
    if (m_active)
      dxvk::war3::gpu_skin::EndNativeFlushTransaction();
    if (m_eventSequence != 0u && m_active) {
      const int64_t end = dxvk::high_resolution_clock::get_counter();
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
      dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
          m_eventSequence,
          dxvk::war3::render::
              WorldObjectsPhase1PairedTimingStage::FlushTransactionEnd,
          end >= begin ? uint64_t(end - begin) : 0u);
    }
  }

  NativeGpuSkinBridgeFlushScope(
      const NativeGpuSkinBridgeFlushScope&) = delete;
  NativeGpuSkinBridgeFlushScope& operator=(
      const NativeGpuSkinBridgeFlushScope&) = delete;

private:
  bool m_active = false;
  uint64_t m_eventSequence = 0u;
};

static bool DisableNativeGpuSkinHookEntry(
    NativeGpuSkinOwnedHook& hook, const char* hookName) {
  if (!hook.created || hook.target == nullptr)
    return true;

  const MH_STATUS disableStatus = MH_DisableHook(hook.target);
  if (disableStatus != MH_OK && disableStatus != MH_ERROR_DISABLED) {
    war3dbg::Print(
        "DXVK War3Hook[GpuSkin]: disable %s failed (status=%d, addr=%p); "
        "keeping trampoline live\n",
        hookName, static_cast<int>(disableStatus), hook.target);
    char line[192] = {};
    std::snprintf(
        line, sizeof(line),
        "DXVK War3Hook[GpuSkin]: disable %s failed status=%d addr=%p; "
        "bridge disabled, trampoline retained",
        hookName, static_cast<int>(disableStatus), hook.target);
    ::dxvk::Logger::info(line);
    return false;
  }
  hook.enabled = false;
  RecordHookInstallState("GpuSkin", hook.name ? hook.name : hookName,
                         hook.target, hook.detour, hook.trampoline,
                         "disabled", false);
  return true;
}

static bool CreateNativeGpuSkinHook(LPVOID target, LPVOID detour,
                                    LPVOID* trampoline,
                                    const char* hookName,
                                    NativeGpuSkinOwnedHook& hook) {
  if (target == nullptr || detour == nullptr || trampoline == nullptr) {
    RecordHookInstallState("GpuSkin", hookName, target, detour, nullptr,
                           "invalid-args", false);
    return false;
  }
  if (hook.created) {
    return hook.target == target && *trampoline != nullptr;
  }

  *trampoline = nullptr;
  const MH_STATUS createStatus = MH_CreateHook(target, detour, trampoline);
  if (createStatus != MH_OK) {
    war3dbg::Print(
        "DXVK War3Hook[GpuSkin]: create %s failed (status=%d, addr=%p)\n",
        hookName, static_cast<int>(createStatus), target);
    char line[176] = {};
    std::snprintf(
        line, sizeof(line),
        "DXVK War3Hook[GpuSkin]: create %s failed status=%d addr=%p",
        hookName, static_cast<int>(createStatus), target);
    ::dxvk::Logger::info(line);
    RecordHookInstallState("GpuSkin", hookName, target, detour,
                           trampoline ? *trampoline : nullptr,
                           "create-failed", false);
    return false;
  }

  hook.name = hookName;
  hook.target = target;
  hook.detour = detour;
  hook.trampoline = *trampoline;
  hook.created = true;
  hook.enabled = false;
  RecordHookInstallState("GpuSkin", hookName, target, detour, *trampoline,
                         "created-disabled", false);
  return *trampoline != nullptr;
}

static bool QueueEnableNativeGpuSkinHooks() {
  NativeGpuSkinOwnedHook* hooks[] = {
      &g_gpuSkinApplyHook, &g_gpuSkinUploadHook, &g_gpuSkinKernelHook};
  bool queued = true;
  for (NativeGpuSkinOwnedHook* hook : hooks) {
    if (!hook->created || hook->target == nullptr) {
      queued = false;
      continue;
    }
    const MH_STATUS status = MH_QueueEnableHook(hook->target);
    if (status != MH_OK) {
      queued = false;
      RecordHookInstallState("GpuSkin", hook->name, hook->target,
                             hook->detour, hook->trampoline,
                             "queue-enable-failed", false);
    }
  }
  if (!queued)
    return false;
  const MH_STATUS applyStatus = MH_ApplyQueued();
  if (applyStatus != MH_OK) {
    for (NativeGpuSkinOwnedHook* hook : hooks) {
      RecordHookInstallState("GpuSkin", hook->name, hook->target,
                             hook->detour, hook->trampoline,
                             "apply-enable-failed", false);
    }
    return false;
  }
  for (NativeGpuSkinOwnedHook* hook : hooks) {
    hook->enabled = true;
    RecordHookInstallState("GpuSkin", hook->name, hook->target,
                           hook->detour, hook->trampoline,
                           "installed", true);
  }
  return true;
}

static bool BestEffortDisableNativeGpuSkinHooks() {
  NativeGpuSkinOwnedHook* hooks[] = {
      &g_gpuSkinApplyHook, &g_gpuSkinUploadHook, &g_gpuSkinKernelHook};
  // Overwrite any partially queued enable state first. ApplyQueued may have
  // exposed only a subset, so every created target is then disabled directly.
  bool queueAccepted = true;
  for (NativeGpuSkinOwnedHook* hook : hooks) {
    if (!hook->created || hook->target == nullptr)
      continue;
    if (MH_QueueDisableHook(hook->target) != MH_OK)
      queueAccepted = false;
  }
  const bool queueApplied = MH_ApplyQueued() == MH_OK;
  bool disabled = queueAccepted && queueApplied;
  disabled = DisableNativeGpuSkinHookEntry(
      g_gpuSkinKernelHook, "GxDeviceD3dSkinCopyKernel") && disabled;
  disabled = DisableNativeGpuSkinHookEntry(
      g_gpuSkinUploadHook, "GxDeviceD3dDynamicVertexUpload") && disabled;
  disabled = DisableNativeGpuSkinHookEntry(
      g_gpuSkinApplyHook, "ApplyDrawStateAndSamplerPair") && disabled;
  return disabled;
}

static void EnterNativeGpuSkinSafetyOnly() {
  g_gpuSkinDetourActivity.admitFlushPins.store(
      false, std::memory_order_release);
  g_gpuSkinDetourActivity.state.store(
      NativeGpuSkinDetourState::SafetyOnly, std::memory_order_release);
  dxvk::war3::gpu_skin::RetainNativeBridgeSafetyObservation();
}

static bool WaitForNativeGpuSkinDetoursToDrain(uint32_t timeoutMs) {
  const ULONGLONG start = ::GetTickCount64();
  ULONGLONG quietSince = 0u;
  for (;;) {
    const auto bridge =
        dxvk::war3::gpu_skin::GetNativeBridgeQuiescenceSnapshot();
    const bool detoursQuiet =
        g_gpuSkinDetourActivity.apply.load(std::memory_order_acquire) == 0u &&
        g_gpuSkinDetourActivity.outerUpload.load(
            std::memory_order_acquire) == 0u &&
        g_gpuSkinDetourActivity.kernel.load(std::memory_order_acquire) == 0u &&
        g_gpuSkinDetourActivity.flush.load(std::memory_order_acquire) == 0u;
    const bool bridgeQuiet = bridge.activeCallbackPins == 0u &&
        bridge.activeFlushTransactions == 0u &&
        bridge.activeDispatchTransactions == 0u &&
        bridge.activeSemanticTransactions == 0u &&
        bridge.activeUploadTransactions == 0u &&
        bridge.activeDipObserverTransactions == 0u &&
        bridge.pendingKernelAuthorizations == 0u &&
        bridge.retirementEventsPending == 0u &&
        bridge.telemetryDeltaPending == 0u &&
        !bridge.telemetryDeltaFaulted &&
        !bridge.resetPending &&
        (!bridge.currentThreadIsObservedRenderThread ||
         bridge.currentThreadTlsQuiescent) &&
        !bridge.retirementQueueFaulted &&
        bridge.poisonRangesOutstanding == 0u;
    const ULONGLONG now = ::GetTickCount64();
    if (detoursQuiet && bridgeQuiet) {
      if (quietSince == 0u)
        quietSince = now;
      if (now - quietSince >= 2u)
        return true;
    } else {
      quietSince = 0u;
    }

    if (timeoutMs != std::numeric_limits<uint32_t>::max() &&
        now - start >= timeoutMs) {
      war3dbg::Print(
          "DXVK War3Hook[GpuSkin]: quiescence timeout "
          "detour(apply=%u outer=%u kernel=%u flush=%u) "
          "tls(flush=%u dispatch=%u semantic=%u upload=%u dip=%u "
          "callbacks=%u) "
          "pending=%llu poison=%llu reset=%u tls=%u telemetry=%u/%u; "
          "retaining hooks/trampolines\n",
          g_gpuSkinDetourActivity.apply.load(std::memory_order_relaxed),
          g_gpuSkinDetourActivity.outerUpload.load(
              std::memory_order_relaxed),
          g_gpuSkinDetourActivity.kernel.load(std::memory_order_relaxed),
          g_gpuSkinDetourActivity.flush.load(std::memory_order_relaxed),
          bridge.activeFlushTransactions,
          bridge.activeDispatchTransactions,
          bridge.activeSemanticTransactions,
          bridge.activeUploadTransactions,
          bridge.activeDipObserverTransactions,
          bridge.activeCallbackPins,
          static_cast<unsigned long long>(
              bridge.pendingKernelAuthorizations),
          static_cast<unsigned long long>(bridge.poisonRangesOutstanding),
          bridge.resetPending ? 1u : 0u,
          bridge.currentThreadTlsQuiescent ? 1u : 0u,
          bridge.telemetryDeltaPending,
          bridge.telemetryDeltaFaulted ? 1u : 0u);
      return false;
    }
    if (!::SwitchToThread())
      ::Sleep(1u);
  }
}

static bool UninstallNativeGpuSkinHookTransaction() {
  // Gate new bypass/callback ingress before disabling any entry. No mutex is
  // held while MinHook suspends threads or while the drain loop waits.
  g_gpuSkinDetourActivity.state.store(
      NativeGpuSkinDetourState::Draining, std::memory_order_release);
  dxvk::war3::gpu_skin::SetNativeBridgeHooksInstalled(false);
  // FlushSortedItems is shared with the render queue and remains enabled.
  // Closing pin admission lets already-entered flush detours drain without
  // taking ownership of that shared hook's lifecycle.
  g_gpuSkinDetourActivity.admitFlushPins.store(
      false, std::memory_order_release);
  const bool disabled = BestEffortDisableNativeGpuSkinHooks();
  if (!disabled || !WaitForNativeGpuSkinDetoursToDrain(
          dxvk::war3::gpu_skin::kNativeBridgeCallbackDrainTimeoutMs)) {
    EnterNativeGpuSkinSafetyOnly();
    return false;
  }

  // D3D9 DIP observation is not owned by the three MinHook targets. Keep its
  // ingress open through the poison-retiring drain above, then close it and
  // drain again so no new device DIP can enter between quiescence and final
  // bridge disable. A failure reopens SafetyOnly observation.
  if (!dxvk::war3::gpu_skin::CloseNativeDipObserverIngressForRemoval() ||
      !WaitForNativeGpuSkinDetoursToDrain(
          dxvk::war3::gpu_skin::kNativeBridgeCallbackDrainTimeoutMs)) {
    EnterNativeGpuSkinSafetyOnly();
    return false;
  }

  // Created hooks and trampolines are process-lifetime. A target that may have
  // been enabled is never removed or cleared, even after a clean drain.
  g_gpuSkinDetourActivity.state.store(
      NativeGpuSkinDetourState::Disabled, std::memory_order_release);
  dxvk::war3::gpu_skin::FinalizeNativeBridgeHooksRemoved();
  return true;
}

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
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookFrameScope(const char *name);
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookDrawScope(const char *name);

static bool War3PreparedSliceProbeRuntimeEnabled() {
  static const bool s_enabled = [] {
    const char* value = std::getenv("DXVK_WAR3_PREPARED_SLICE_PROBE");
    return value != nullptr && std::strcmp(value, "0") != 0;
  }();
  return s_enabled;
}

static uint32_t War3PreparedSliceProbeSampleRate() {
  static const uint32_t s_sampleRate = [] {
    const char* value =
        std::getenv("DXVK_WAR3_PREPARED_SLICE_PROBE_SAMPLE_RATE");
    if (value == nullptr || value[0] == '\0')
      return 32u;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed == 0ul)
      return 32u;
    return static_cast<uint32_t>((std::min)(parsed, 4096ul));
  }();
  return s_sampleRate;
}

static void TryPublishPreparedSliceProbe(void* batchContext) {
  if (!War3PreparedSliceProbeRuntimeEnabled())
    return;
  const auto drawContext =
      dxvk::war3::render::GetCurrentDrawDispatchContext();
  const bool contextReady =
      drawContext.valid && drawContext.renderablePart != nullptr;
  const bool interested =
      contextReady &&
      dxvk::war3::render::IsCurrentDrawPreparedSliceInterested(
          drawContext.renderablePart, drawContext.layerIndex);
  thread_local uint32_t s_probeCallCounter = 0u;
  const uint32_t sampleRate = War3PreparedSliceProbeSampleRate();
  if (!interested && sampleRate > 1u &&
      ((++s_probeCallCounter % sampleRate) != 0u))
    return;

  void* preparedMeta = nullptr;
  void* preparedBacking = nullptr;
  uint32_t primitiveType = 0u;
  uint32_t preparedCount = 0u;
  const bool metaReadable =
      batchContext != nullptr &&
      dxvk::war3::SafeReadPtrFast(batchContext, 0xCCu, preparedMeta) &&
      preparedMeta != nullptr &&
      dxvk::war3::SafeReadU32Fast(preparedMeta, 0u, primitiveType) &&
      dxvk::war3::SafeReadU32Fast(preparedMeta, 4u, preparedCount);
  const bool backingReadable =
      batchContext != nullptr &&
      dxvk::war3::SafeReadPtrFast(batchContext, 0xE0u, preparedBacking) &&
      preparedBacking != nullptr &&
      dxvk::war3::IsReadableRangeFast(preparedBacking, 8u);

  dxvk::war3::render::NoteCurrentDrawPreparedSliceProbe(contextReady,
                                                        backingReadable);
  if (!contextReady || !metaReadable || !backingReadable ||
      primitiveType == 0u || preparedCount == 0u) {
    return;
  }

  uint32_t backingWord0 = 0u;
  uint32_t backingWord1 = 0u;
  std::memcpy(&backingWord0, preparedBacking, sizeof(backingWord0));
  std::memcpy(&backingWord1,
              static_cast<const uint8_t*>(preparedBacking) + 4u,
              sizeof(backingWord1));
  uint64_t preparedHash = bit::fnv1a_init();
  preparedHash = bit::fnv1a_iter(preparedHash, primitiveType);
  preparedHash = bit::fnv1a_iter(preparedHash, preparedCount);
  preparedHash = bit::fnv1a_iter(preparedHash, backingWord0);
  preparedHash = bit::fnv1a_iter(preparedHash, backingWord1);

  dxvk::war3::render::CurrentDrawPreparedSliceRecord record = {};
  record.known = true;
  record.sceneNode = drawContext.sceneNode;
  record.renderablePart = drawContext.renderablePart;
  record.layerIndex = drawContext.layerIndex;
  record.primitiveType = primitiveType;
  record.preparedCount = preparedCount;
  record.preparedHash = preparedHash;
  dxvk::war3::render::PublishCurrentDrawPreparedSlice(record);
}

class CurrentDrawDispatchScope {
public:
  CurrentDrawDispatchScope(void* sceneNode,
                           void* renderablePart,
                           uint32_t layerIndex)
      : m_previous(dxvk::war3::render::PushCurrentDrawDispatchContext(
            sceneNode, renderablePart, layerIndex)) {
  }

  ~CurrentDrawDispatchScope() {
    dxvk::war3::render::RestoreCurrentDrawDispatchContext(m_previous);
  }

  CurrentDrawDispatchScope(const CurrentDrawDispatchScope&) = delete;
  CurrentDrawDispatchScope& operator=(const CurrentDrawDispatchScope&) = delete;

private:
  dxvk::war3::render::CurrentDrawDispatchContext m_previous = {};
};

// 诊断门在进程期只解析一次；必须同时满足 detail 级和显式环境变量，
// 普通 PERF_LEVEL=0/1 以及未显式启用的 level 2 都保持关闭。
static inline bool War3PublishVisibleBreakdownRuntimeEnabled() noexcept {
  if constexpr (!dxvk::war3::internal::kNativePerfDetailHookTimingEnabled)
    return false;
  static const bool enabled = [] {
    if (dxvk::war3::internal::War3PerfHookLevel() < 2)
      return false;
    const char* value =
        std::getenv("DXVK_WAR3_PERF_PUBLISH_VISIBLE_BREAKDOWN");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

// 周期同样只解析一次。非法值回退 8，4096 上限避免误配置导致权重
// 乘法和长窗口统计失去可读性。
static inline uint32_t War3PublishVisibleBreakdownSamplePeriod() noexcept {
  static const uint32_t period = [] {
    constexpr uint32_t kDefaultPeriod = 8u;
    const char* value =
        std::getenv("DXVK_WAR3_PERF_PUBLISH_VISIBLE_SAMPLE_PERIOD");
    if (value == nullptr || value[0] == '\0')
      return kDefaultPeriod;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0ul)
      return kDefaultPeriod;
    return static_cast<uint32_t>((std::min)(parsed, 4096ul));
  }();
  return period;
}

struct War3PublishVisibleSafeCopyConfig {
  bool enabled = false;
  bool verifierEnabled = false;
  bool assertOnMismatch = false;
  uint32_t verifierSamplePeriod = 256u;
};

static inline const War3PublishVisibleSafeCopyConfig&
War3PublishVisibleSafeCopyRuntimeConfig() noexcept {
  // SafeCopy passed a period-1 shadow verifier over 670k production calls and
  // a same-DLL reverse A/B/A/B gate. It is now the production default; exact
  // env value 0 remains the immediate rollback to historical SafeReadPtrFast.
  static const War3PublishVisibleSafeCopyConfig config = [] {
    const auto readExactFlag = [](const char* name) {
      const char* value = std::getenv(name);
      return value && value[0] == '1' && value[1] == '\0';
    };
    const auto readDefaultOnFlag = [](const char* name) {
      const char* value = std::getenv(name);
      return !(value && value[0] == '0' && value[1] == '\0');
    };
    const auto readPeriod = [](const char* name, uint32_t fallback) {
      const char* value = std::getenv(name);
      if (value == nullptr || value[0] == '\0')
        return fallback;
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (end == value || *end != '\0' || parsed == 0ul)
        return fallback;
      return static_cast<uint32_t>((std::min)(parsed, 4096ul));
    };

    War3PublishVisibleSafeCopyConfig result = {};
    result.enabled =
        readDefaultOnFlag("DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY");
    result.assertOnMismatch = readExactFlag(
        "DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_ASSERT");
    result.verifierEnabled =
        readExactFlag("DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY") ||
        result.assertOnMismatch;
    result.verifierSamplePeriod = readPeriod(
        "DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_SAMPLE_PERIOD", 256u);
    return result;
  }();
  return config;
}

struct PublishVisiblePointerReadResult {
  void* meshData = nullptr;
  void* sceneNode = nullptr;
  bool copySucceeded = false;
  bool meshReadSucceeded = false;
  bool sceneReadAttempted = false;
  bool sceneReadSucceeded = false;

  bool earlyReturn() const noexcept {
    return meshData == nullptr && sceneNode == nullptr;
  }
};

static inline PublishVisiblePointerReadResult
ReadPublishVisiblePointersLegacy(
    void* renderablePart, void* presetSceneNode) noexcept {
  PublishVisiblePointerReadResult result = {};
  result.sceneNode = presetSceneNode;
  result.meshReadSucceeded = dxvk::war3::SafeReadPtrFast(
      renderablePart, dxvk::war3::RenderablePartFieldOffsets::MeshData,
      result.meshData);
  if (result.sceneNode == nullptr) {
    result.sceneReadAttempted = true;
    void* partSceneNode = nullptr;
    result.sceneReadSucceeded = dxvk::war3::SafeReadPtrFast(
        renderablePart,
        dxvk::war3::RenderablePartFieldOffsets::SceneNode,
        partSceneNode);
    result.sceneNode = partSceneNode;
  }
  return result;
}

static inline PublishVisiblePointerReadResult
ReadPublishVisiblePointersCandidate(
    void* renderablePart, void* presetSceneNode) noexcept {
  PublishVisiblePointerReadResult result = {};
  result.sceneNode = presetSceneNode;
  const auto* partBytes = static_cast<const uint8_t*>(renderablePart);
  if (result.sceneNode == nullptr) {
    constexpr size_t kSpanBegin =
        dxvk::war3::RenderablePartFieldOffsets::MeshData;
    constexpr size_t kSpanEnd =
        dxvk::war3::RenderablePartFieldOffsets::SceneNode + sizeof(void*);
    std::array<uint8_t, kSpanEnd - kSpanBegin> fields = {};
    result.copySucceeded = dxvk::war3::SafeCopy(
        fields.data(), partBytes + kSpanBegin, fields.size());
    if (result.copySucceeded) {
      std::memcpy(
          &result.meshData,
          fields.data() +
              (dxvk::war3::RenderablePartFieldOffsets::MeshData -
               kSpanBegin),
          sizeof(result.meshData));
      std::memcpy(
          &result.sceneNode,
          fields.data() +
              (dxvk::war3::RenderablePartFieldOffsets::SceneNode -
               kSpanBegin),
          sizeof(result.sceneNode));
      result.meshReadSucceeded = true;
      result.sceneReadAttempted = true;
      result.sceneReadSucceeded = true;
    }
  } else {
    void* copiedMeshData = nullptr;
    result.copySucceeded = dxvk::war3::SafeCopy(
        &copiedMeshData,
        partBytes + dxvk::war3::RenderablePartFieldOffsets::MeshData,
        sizeof(copiedMeshData));
    if (result.copySucceeded) {
      result.meshData = copiedMeshData;
      result.meshReadSucceeded = true;
    }
  }

  if (!result.copySucceeded) {
    PublishVisiblePointerReadResult fallback =
        ReadPublishVisiblePointersLegacy(renderablePart, presetSceneNode);
    fallback.copySucceeded = false;
    return fallback;
  }
  return result;
}

static inline uint32_t PublishVisibleSafeCopyMismatchMask(
    const PublishVisiblePointerReadResult& candidate,
    const PublishVisiblePointerReadResult& legacy) noexcept {
  uint32_t mask = PublishVisibleSafeCopyMismatchNone;
  if (candidate.meshData != legacy.meshData)
    mask |= PublishVisibleSafeCopyMismatchMeshData;
  if (candidate.sceneNode != legacy.sceneNode)
    mask |= PublishVisibleSafeCopyMismatchSceneNode;
  if (candidate.earlyReturn() != legacy.earlyReturn())
    mask |= PublishVisibleSafeCopyMismatchEarlyReturn;
  return mask;
}

static inline bool SamePublishVisibleReadObservation(
    const PublishVisiblePointerReadResult& lhs,
    const PublishVisiblePointerReadResult& rhs) noexcept {
  return lhs.meshData == rhs.meshData &&
      lhs.sceneNode == rhs.sceneNode &&
      lhs.copySucceeded == rhs.copySucceeded &&
      lhs.meshReadSucceeded == rhs.meshReadSucceeded &&
      lhs.sceneReadAttempted == rhs.sceneReadAttempted &&
      lhs.sceneReadSucceeded == rhs.sceneReadSucceeded &&
      lhs.earlyReturn() == rhs.earlyReturn();
}

static std::mutex g_publishVisibleSafeCopyVerifierMutex;
static PublishVisibleSafeCopyVerifierStats
    g_publishVisibleSafeCopyVerifierStats;
static thread_local PublishVisibleSafeCopyVerifierStats
    t_publishVisibleSafeCopyVerifierStats;
static thread_local uint64_t
    t_publishVisibleSafeCopyVerifierOrdinal = 0u;

static void MergePublishVisibleSafeCopyVerifierStats(
    PublishVisibleSafeCopyVerifierStats& destination,
    const PublishVisibleSafeCopyVerifierStats& source) noexcept {
  destination.attempts += source.attempts;
  destination.scenePreset += source.scenePreset;
  destination.sceneNullInput += source.sceneNullInput;
  destination.copySuccess += source.copySuccess;
  destination.copyFailure += source.copyFailure;
  destination.candidateEarlyReturn += source.candidateEarlyReturn;
  destination.legacyEarlyReturn += source.legacyEarlyReturn;
  destination.rereadCount += source.rereadCount;
  destination.initialMismatchCount += source.initialMismatchCount;
  destination.stableMismatchCount += source.stableMismatchCount;
  destination.unstableCount += source.unstableCount;
  destination.initialMismatchMaskOr |= source.initialMismatchMaskOr;
  destination.stableMismatchMaskOr |= source.stableMismatchMaskOr;
  for (size_t i = 0u;
       i < kPublishVisibleSafeCopyMismatchMaskCount; ++i) {
    destination.initialMismatchMaskCounts[i] +=
        source.initialMismatchMaskCounts[i];
    destination.stableMismatchMaskCounts[i] +=
        source.stableMismatchMaskCounts[i];
  }
}

static void FlushPublishVisibleSafeCopyVerifierStats(
    bool force) noexcept {
  constexpr uint64_t kFlushBatchAttempts = 64u;
  auto& pending = t_publishVisibleSafeCopyVerifierStats;
  if (pending.attempts == 0u ||
      (!force && pending.attempts < kFlushBatchAttempts)) {
    return;
  }
  std::lock_guard<std::mutex> lock(
      g_publishVisibleSafeCopyVerifierMutex);
  MergePublishVisibleSafeCopyVerifierStats(
      g_publishVisibleSafeCopyVerifierStats, pending);
  pending = {};
}

static void RecordPublishVisibleSafeCopyVerification(
    void* renderablePart,
    void* presetSceneNode,
    const PublishVisiblePointerReadResult& candidate,
    const PublishVisiblePointerReadResult& legacy) noexcept {
  auto& stats = t_publishVisibleSafeCopyVerifierStats;
  ++stats.attempts;
  if (presetSceneNode != nullptr)
    ++stats.scenePreset;
  else
    ++stats.sceneNullInput;
  if (candidate.copySucceeded)
    ++stats.copySuccess;
  else
    ++stats.copyFailure;
  if (candidate.earlyReturn())
    ++stats.candidateEarlyReturn;
  if (legacy.earlyReturn())
    ++stats.legacyEarlyReturn;

  const uint32_t initialMask =
      PublishVisibleSafeCopyMismatchMask(candidate, legacy);
  stats.initialMismatchMaskOr |= initialMask;
  ++stats.initialMismatchMaskCounts[initialMask];

  bool stableMismatch = false;
  if (initialMask == PublishVisibleSafeCopyMismatchNone) {
    ++stats.stableMismatchMaskCounts[
        PublishVisibleSafeCopyMismatchNone];
  } else {
    ++stats.initialMismatchCount;
    ++stats.rereadCount;
    const PublishVisiblePointerReadResult candidateReread =
        ReadPublishVisiblePointersCandidate(
            renderablePart, presetSceneNode);
    const PublishVisiblePointerReadResult legacyReread =
        ReadPublishVisiblePointersLegacy(
            renderablePart, presetSceneNode);
    const uint32_t rereadMask =
        PublishVisibleSafeCopyMismatchMask(
            candidateReread, legacyReread);
    const bool observationsStable =
        rereadMask == initialMask &&
        SamePublishVisibleReadObservation(candidate, candidateReread) &&
        SamePublishVisibleReadObservation(legacy, legacyReread);
    if (observationsStable) {
      stableMismatch = true;
      ++stats.stableMismatchCount;
      stats.stableMismatchMaskOr |= rereadMask;
      ++stats.stableMismatchMaskCounts[rereadMask];
    } else {
      ++stats.unstableCount;
    }
  }

  // A mismatch must never remain in another thread's pending batch while a
  // report claims zero. Ordinary exact samples amortize publication.
  FlushPublishVisibleSafeCopyVerifierStats(
      initialMask != PublishVisibleSafeCopyMismatchNone);
  if (stableMismatch &&
      War3PublishVisibleSafeCopyRuntimeConfig().assertOnMismatch) {
    // This verifier is also used in release-style unattended gates where
    // NDEBUG may compile assert() away. A stable semantic mismatch must make
    // the gate fail deterministically instead of merely being counted.
    std::abort();
  }
}

static inline PublishVisiblePointerReadResult
ReadPublishVisiblePointers(
    void* renderablePart, void* presetSceneNode) noexcept {
  const auto& config = War3PublishVisibleSafeCopyRuntimeConfig();
  if (config.verifierEnabled &&
      War3HotHookShouldSample(
          ++t_publishVisibleSafeCopyVerifierOrdinal,
          config.verifierSamplePeriod)) {
    // Both implementations publish only into local temporaries. Production
    // selection happens after comparison, so verifier mode cannot leak the
    // non-selected reader into the visible registry.
    const PublishVisiblePointerReadResult candidate =
        ReadPublishVisiblePointersCandidate(
            renderablePart, presetSceneNode);
    const PublishVisiblePointerReadResult legacy =
        ReadPublishVisiblePointersLegacy(
            renderablePart, presetSceneNode);
    RecordPublishVisibleSafeCopyVerification(
        renderablePart, presetSceneNode, candidate, legacy);
    return config.enabled ? candidate : legacy;
  }
  return config.enabled
      ? ReadPublishVisiblePointersCandidate(
            renderablePart, presetSceneNode)
      : ReadPublishVisiblePointersLegacy(
            renderablePart, presetSceneNode);
}

PublishVisibleSafeCopyVerifierStats
QueryPublishVisibleSafeCopyVerifierStats() noexcept {
  FlushPublishVisibleSafeCopyVerifierStats(true);
  PublishVisibleSafeCopyVerifierStats result = {};
  {
    std::lock_guard<std::mutex> lock(
        g_publishVisibleSafeCopyVerifierMutex);
    result = g_publishVisibleSafeCopyVerifierStats;
  }
  const auto& config = War3PublishVisibleSafeCopyRuntimeConfig();
  result.productionEnabled = config.enabled;
  result.enabled = config.verifierEnabled;
  result.assertOnMismatch = config.assertOnMismatch;
  result.samplePeriod = config.verifierSamplePeriod;
  return result;
}

// false 实例化只执行原业务 lambda；true 实例化才建立固定 ID/QPC
// 子节点。模板分流避免默认路径在八个边界重复检查运行时开关。
template <bool TimingEnabled, typename Fn>
static inline void RunPublishVisibleBreakdownSegment(
    War3HotHookId id, uint32_t sampleWeight, Fn&& fn) {
  if constexpr (TimingEnabled) {
    War3HotHookCallTiming timing(
        id, War3HotHookPreselectedSample{sampleWeight});
    fn();
  } else {
    (void)id;
    (void)sampleWeight;
    fn();
  }
}

// 两个模板实例保持完全相同的业务顺序与早退语义；差异只在编译期
// 是否包裹固定 ID 计时，因此默认实例没有字符串、TLS 或 QPC 工作。
template <bool TimingEnabled>
static inline void PublishVisibleRenderableFromDispatchBody(
    void* sceneNode,
    void* renderablePart,
    uint32_t layerIndex,
    void* layerState,
    uint32_t sampleWeight) {
  if (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled)
    return;
  if (!dxvk::war3::runtime::IsWar3RuntimeModuleEnabled(
          dxvk::war3::runtime::War3RuntimeModule::SemanticData) ||
      !dxvk::war3::internal::kWar3RuntimeConfigSemanticFrameRegistriesEffective)
    return;
  if (renderablePart == nullptr)
    return;

  void* meshData = nullptr;
  RunPublishVisibleBreakdownSegment<TimingEnabled>(
      War3HotHookId::PublishVisibleSafeRead, sampleWeight, [&] {
    const PublishVisiblePointerReadResult pointers =
        ReadPublishVisiblePointers(renderablePart, sceneNode);
    meshData = pointers.meshData;
    sceneNode = pointers.sceneNode;
  });

  if (sceneNode == nullptr && meshData == nullptr)
    return;

  const War3BatchTag tag = War3RenderState::GetTlsBatchTag();
  dxvk::war3::render::RenderObjectIdentitySnapshot identity = {};
  identity.sceneNode = sceneNode;
  RunPublishVisibleBreakdownSegment<TimingEnabled>(
      War3HotHookId::PublishVisibleTlsIdentityMerge, sampleWeight, [&] {
    MergeShadowSemanticTlsIdentity(renderablePart, sceneNode, identity);
    ClassifyWorldTagIdentity(tag, identity);
  });

  if (!HasHotVisibleUnitIdentity(identity)) {
    RunPublishVisibleBreakdownSegment<TimingEnabled>(
        War3HotHookId::PublishVisibleCurrentIdentity, sampleWeight, [&] {
      dxvk::war3::render::RenderObjectIdentitySnapshot resolvedIdentity = {};
      if (dxvk::war3::render::TryResolveCurrentRenderObjectIdentity(
              sceneNode, resolvedIdentity)) {
        MergeRenderObjectIdentity(identity, resolvedIdentity);
      }
      if (identity.sceneNode == nullptr)
        identity.sceneNode = sceneNode;
    });
  }

  if (!HasHotVisibleUnitIdentity(identity)) {
    RunPublishVisibleBreakdownSegment<TimingEnabled>(
        War3HotHookId::PublishVisibleRenderQueueCache, sampleWeight, [&] {
      dxvk::war3::render::RenderObjectIdentitySnapshot cachedIdentity = {};
      if (dxvk::war3::render::RenderQueueTracker::instance()
              .GetCachedObjectIdentity(renderablePart, cachedIdentity)) {
        MergeRenderObjectIdentity(identity, cachedIdentity);
      }
    });
  }

  if (!HasHotVisibleUnitIdentity(identity)) {
    RunPublishVisibleBreakdownSegment<TimingEnabled>(
        War3HotHookId::PublishVisiblePriorVisibleLookup, sampleWeight, [&] {
      dxvk::war3::render::VisibleRenderableRecord prior = {};
      if (dxvk::war3::render::VisibleRenderableRegistry::instance()
              .queryByRenderablePart(renderablePart, prior)) {
        MergeRenderObjectIdentity(identity, prior.identity);
      }
    });
  }

  RunPublishVisibleBreakdownSegment<TimingEnabled>(
      War3HotHookId::PublishVisibleUnitFill, sampleWeight, [&] {
    TryFillUnitIdentityFromWorldObjectEntry(identity);
    if (identity.rawcode == 0u || identity.flags5C == 0u ||
        identity.jHandle == 0u ||
        identity.kind == dxvk::war3::render::ObjectKind::Unknown) {
      TryFillUnitIdentityFromUnitPtr(identity);
    }
    ClassifyWorldTagIdentity(tag, identity);
  });

  dxvk::war3::render::VisibleRenderableRecord record = {};
  RunPublishVisibleBreakdownSegment<TimingEnabled>(
      War3HotHookId::PublishVisibleRecordBuild, sampleWeight, [&] {
    record.queueKind =
        dxvk::war3::render::VisibleRenderableQueueKind::MainQueue;
    record.payload = renderablePart;
    record.renderablePart = renderablePart;
    record.sceneNode = sceneNode;
    record.meshData = meshData;
    record.layerState = layerState;
    record.layerIndex = layerIndex;
    record.stage = static_cast<int16_t>(War3RenderState::GetStage());
    const auto& tlsSemantic = War3RenderState::GetTlsShadowSemanticState();
    record.pathBlocker =
        tlsSemantic.pathBlocker ||
        dxvk::war3::internal::IsPathBlockerFourCc(identity.rawcode);
    record.identity = identity;
  });
  RunPublishVisibleBreakdownSegment<TimingEnabled>(
      War3HotHookId::PublishVisibleRegistryRegister, sampleWeight, [&] {
    dxvk::war3::render::VisibleRenderableRegistry::instance()
        .registerSemanticCandidate(record);
  });
}

static void PublishVisibleRenderableFromDispatch(
    void* sceneNode,
    void* renderablePart,
    uint32_t layerIndex,
    void* layerState) {
  // 默认路径只付一次进程期静态 bool 分支；不读取 TLS、不推进 ordinal、
  // 不查 section，也不触发 QPC。显式诊断时以整次调用统一抽样，保证八个
  // 子阶段来自同一批调用且能与根节点闭合。
  if (War3PublishVisibleBreakdownRuntimeEnabled()) {
    static thread_local uint64_t sampleOrdinal = 0u;
    const uint32_t samplePeriod =
        War3PublishVisibleBreakdownSamplePeriod();
    if (War3HotHookShouldSample(++sampleOrdinal, samplePeriod)) {
      War3HotHookCallTiming rootTiming(
          War3HotHookId::PublishVisible,
          War3HotHookPreselectedSample{samplePeriod});
      PublishVisibleRenderableFromDispatchBody<true>(
          sceneNode, renderablePart, layerIndex, layerState, samplePeriod);
      return;
    }
  }
  PublishVisibleRenderableFromDispatchBody<false>(
      sceneNode, renderablePart, layerIndex, layerState, 1u);
}

static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookCpuScope(const char *name) {
  // detail 级：PERF_LEVEL=2 时全开（原为编译期常量，现改为运行时分级）。
  if constexpr (dxvk::war3::internal::kNativePerfDetailHookTimingEnabled) {
    if (dxvk::war3::internal::War3PerfHookLevel() >= 2)
      return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

// 帧级低频 scope：Prepare/RenderScene/WorldDispatch/RenderGroup/Flush 等，
// 每帧 < 50 次 QPC 对，PERF_LEVEL>=1 时常开。
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookFrameScope(const char *name) {
  if constexpr (dxvk::war3::internal::kNativePerfFrameHookTimingEnabled) {
    if (dxvk::war3::internal::War3PerfHookLevel() >= 1)
      return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

static bool War3TransparentDispatchTimingHooksRuntimeEnabled() {
  // The five native transparent-dispatch entry points do not share one ABI.
  // Keep their timing detours opt-in until every signature has passed an
  // isolated per-type crash gate. The enclosing FlushTransparent hook remains
  // enabled, so the queue still has a stable aggregate timing boundary.
  if constexpr (!dxvk::war3::internal::kNativePerfDetailHookTimingEnabled)
    return false;
  static const bool enabled = [] {
    const char* value =
        std::getenv("DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

#define WAR3_RENDERPERF_WORLD_PREPARE_DEEP_HOOKS(X)                           \
  X(RenderPerfWorldPrepareCameraBuildFrustum,                                  \
    "WorldPrepare_CameraBuildFrustum", worldPrepareCameraBuildFrustum,          \
    Hook_WorldPrepare_CameraBuildFrustum,                                      \
    g_trampolineWorldPrepareCameraBuildFrustum,                                \
    "Hook_WorldPrepare_CameraBuildFrustum")                                    \
  X(RenderPerfWorldPrepareTerrainShadowFlush,                                  \
    "WorldPrepare_TerrainShadowFlush", worldPrepareTerrainShadowFlush,          \
    Hook_WorldPrepare_TerrainShadowFlush,                                      \
    g_trampolineWorldPrepareTerrainShadowFlush,                                \
    "Hook_WorldPrepare_TerrainShadowFlush")                                    \
  X(RenderPerfWorldPrepareTerrainExtraPass,                                    \
    "WorldPrepare_TerrainExtraPass", worldPrepareTerrainExtraPass,              \
    Hook_WorldPrepare_TerrainExtraPass,                                        \
    g_trampolineWorldPrepareTerrainExtraPass,                                  \
    "Hook_WorldPrepare_TerrainExtraPass")                                      \
  X(RenderPerfWorldPrepareShadowProjectorFlush,                                \
    "WorldPrepare_ShadowProjectorFlush", worldPrepareShadowProjectorFlush,      \
    Hook_WorldPrepare_ShadowProjectorFlush,                                    \
    g_trampolineWorldPrepareShadowProjectorFlush,                              \
    "Hook_WorldPrepare_ShadowProjectorFlush")                                  \
  X(RenderPerfWorldPrepareTargetIndicatorRingAdvance,                          \
    "WorldPrepare_TargetIndicatorRingAdvance",                                 \
    worldPrepareTargetIndicatorRingAdvance,                                    \
    Hook_WorldPrepare_TargetIndicatorRingAdvance,                              \
    g_trampolineWorldPrepareTargetIndicatorRingAdvance,                        \
    "Hook_WorldPrepare_TargetIndicatorRingAdvance")                            \
  X(RenderPerfWorldPrepareCinematicFilterTimeAdvance,                          \
    "WorldPrepare_CinematicFilterTimeAdvance",                                 \
    worldPrepareCinematicFilterTimeAdvance,                                    \
    Hook_WorldPrepare_CinematicFilterTimeAdvance,                              \
    g_trampolineWorldPrepareCinematicFilterTimeAdvance,                        \
    "Hook_WorldPrepare_CinematicFilterTimeAdvance")                            \
  X(RenderPerfWorldPrepareRuntimeFlagClockAdvance3B8760,                       \
    "WorldPrepare_RuntimeFlagClockAdvance3B8760",                              \
    worldPrepareRuntimeFlagClockAdvance3B8760,                                 \
    Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760,                           \
    g_trampolineWorldPrepareRuntimeFlagClockAdvance3B8760,                     \
    "Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760")

#define WAR3_RENDERPERF_WORLD_PREPARE_RESIDUAL_HOOKS(X)                        \
  X(RenderPerfWorldPrepareFlushDeferredSelectionObjects,                       \
    "WorldPrepare_FlushDeferredSelectionObjects",                              \
    worldPrepareFlushDeferredSelectionObjects,                                 \
    Hook_WorldPrepare_FlushDeferredSelectionObjects,                           \
    g_trampolineWorldPrepareFlushDeferredSelectionObjects,                     \
    "Hook_WorldPrepare_FlushDeferredSelectionObjects")                         \
  X(RenderPerfWorldPrepareGlobalRenderCallbackPass,                            \
    "WorldPrepare_GlobalRenderCallbackPass",                                   \
    worldPrepareGlobalRenderCallbackPass,                                      \
    Hook_WorldPrepare_GlobalRenderCallbackPass,                                \
    g_trampolineWorldPrepareGlobalRenderCallbackPass,                          \
    "Hook_WorldPrepare_GlobalRenderCallbackPass")                              \
  X(RenderPerfWorldPrepareRenderWaypointIndicators,                            \
    "WorldPrepare_RenderWaypointIndicators",                                   \
    worldPrepareRenderWaypointIndicators,                                      \
    Hook_WorldPrepare_RenderWaypointIndicators,                                \
    g_trampolineWorldPrepareRenderWaypointIndicators,                          \
    "Hook_WorldPrepare_RenderWaypointIndicators")

#define WAR3_RENDERPERF_WORLD_PREPARE_CORE_HOOKS(X)                            \
  X(RenderPerfWorldPrepareFrameUpdateGate, "WorldPrepare_FrameUpdateGate",      \
    worldPrepareFrameUpdateGate, Hook_WorldPrepare_FrameUpdateGate,             \
    g_trampolineWorldPrepareFrameUpdateGate,                                   \
    "Hook_WorldPrepare_FrameUpdateGate")                                       \
  X(RenderPerfWorldPrepareGameUiFrameSync, "WorldPrepare_GameUiFrameSync",      \
    worldPrepareGameUiFrameSync, Hook_WorldPrepare_GameUiFrameSync,             \
    g_trampolineWorldPrepareGameUiFrameSync,                                   \
    "Hook_WorldPrepare_GameUiFrameSync")                                       \
  X(RenderPerfWorldPrepareUpdateIndicatorAnchor,                               \
    "WorldPrepare_UpdateIndicatorAnchor", worldPrepareUpdateIndicatorAnchor,    \
    Hook_WorldPrepare_UpdateIndicatorAnchor,                                   \
    g_trampolineWorldPrepareUpdateIndicatorAnchor,                             \
    "Hook_WorldPrepare_UpdateIndicatorAnchor")                                 \
  X(RenderPerfWorldPrepareCameraAdvance, "WorldPrepare_CameraAdvance",          \
    worldPrepareCameraAdvance, Hook_WorldPrepare_CameraAdvance,                 \
    g_trampolineWorldPrepareCameraAdvance,                                     \
    "Hook_WorldPrepare_CameraAdvance")                                         \
  X(RenderPerfWorldPrepareCameraPrepareConstants,                              \
    "WorldPrepare_CameraPrepareConstants",                                     \
    worldPrepareCameraPrepareConstants,                                        \
    Hook_WorldPrepare_CameraPrepareConstants,                                  \
    g_trampolineWorldPrepareCameraPrepareConstants,                            \
    "Hook_WorldPrepare_CameraPrepareConstants")                                \
  X(RenderPerfWorldPrepareViewProjPrepare, "WorldPrepare_ViewProjPrepare",      \
    worldPrepareViewProjPrepare, Hook_WorldPrepare_ViewProjPrepare,             \
    g_trampolineWorldPrepareViewProjPrepare,                                   \
    "Hook_WorldPrepare_ViewProjPrepare")                                       \
  X(RenderPerfWorldPrepareSceneQueryFlushSync,                                 \
    "WorldPrepare_SceneQueryFlushSync", worldPrepareSceneQueryFlushSync,        \
    Hook_WorldPrepare_SceneQueryFlushSync,                                     \
    g_trampolineWorldPrepareSceneQueryFlushSync,                               \
    "Hook_WorldPrepare_SceneQueryFlushSync")                                   \
  X(RenderPerfWorldPrepareFixedPointRemap, "WorldPrepare_FixedPointRemap",      \
    worldPrepareFixedPointRemap, Hook_WorldPrepare_FixedPointRemap,             \
    g_trampolineWorldPrepareFixedPointRemap,                                   \
    "Hook_WorldPrepare_FixedPointRemap")                                       \
  X(RenderPerfWorldPreparePostVisibilityGlobalAdvanceA,                        \
    "WorldPrepare_PostVisibilityGlobalAdvanceA",                               \
    worldPreparePostVisibilityGlobalAdvanceA,                                  \
    Hook_WorldPrepare_PostVisibilityGlobalAdvanceA,                            \
    g_trampolineWorldPreparePostVisibilityGlobalAdvanceA,                      \
    "Hook_WorldPrepare_PostVisibilityGlobalAdvanceA")                          \
  X(RenderPerfWorldPreparePostVisibilityFrameAnchorUpdate,                     \
    "WorldPrepare_PostVisibility_FrameAnchorUpdate",                           \
    worldPreparePostVisibilityFrameAnchorUpdate,                               \
    Hook_WorldPrepare_PostVisibility_FrameAnchorUpdate,                        \
    g_trampolineWorldPreparePostVisibilityFrameAnchorUpdate,                   \
    "Hook_WorldPrepare_PostVisibility_FrameAnchorUpdate")                      \
  X(RenderPerfWorldPreparePostVisibilityFrameAnchorVisibilityQuery,            \
    "WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery",                  \
    worldPreparePostVisibilityFrameAnchorVisibilityQuery,                      \
    Hook_WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery,               \
    g_trampolineWorldPreparePostVisibilityFrameAnchorVisibilityQuery,          \
    "Hook_WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery")             \
  X(RenderPerfWorldPreparePostVisibilityGlobalAdvanceB,                        \
    "WorldPrepare_PostVisibilityGlobalAdvanceB",                               \
    worldPreparePostVisibilityGlobalAdvanceB,                                  \
    Hook_WorldPrepare_PostVisibilityGlobalAdvanceB,                            \
    g_trampolineWorldPreparePostVisibilityGlobalAdvanceB,                      \
    "Hook_WorldPrepare_PostVisibilityGlobalAdvanceB")                          \
  X(RenderPerfWorldPrepareVisibilityTailAdvanceA,                              \
    "WorldPrepare_VisibilityTailAdvanceA", worldPrepareVisibilityTailAdvanceA,  \
    Hook_WorldPrepare_VisibilityTailAdvanceA,                                  \
    g_trampolineWorldPrepareVisibilityTailAdvanceA,                            \
    "Hook_WorldPrepare_VisibilityTailAdvanceA")                                \
  X(RenderPerfWorldPrepareVisibilityTailAdvanceB,                              \
    "WorldPrepare_VisibilityTailAdvanceB", worldPrepareVisibilityTailAdvanceB,  \
    Hook_WorldPrepare_VisibilityTailAdvanceB,                                  \
    g_trampolineWorldPrepareVisibilityTailAdvanceB,                            \
    "Hook_WorldPrepare_VisibilityTailAdvanceB")

#define WAR3_RENDERPERF_RENDERQUEUE_DEEP_HOOKS(X)                              \
  X(RenderPerfRenderQueueStageUpdate, "RenderQueue_StageUpdate", rqStageUpdate, \
    Hook_RenderQueueStageUpdateTiming, g_trampolineStageUpdateTiming,           \
    "Hook_RenderQueue_StageUpdate")                                            \
  X(RenderPerfRenderQueueFlushTransparent,                                     \
    "RenderQueue_FlushTransparent", rqFlushTransparent,                        \
    Hook_FlushTransparentTiming, g_trampolineFlushTransparentTiming,            \
    "Hook_RenderQueue_FlushTransparent")

#define WAR3_RENDERPERF_TRANSPARENT_DISPATCH_HOOKS(X)                          \
  X(RenderPerfTransparentDispatchType0,                                        \
    "TransparentDispatch_Type0_RenderBatch", rqTransparentDispatchType0,        \
    Hook_TransparentDispatchType0Timing,                                       \
    g_trampolineTransparentDispatchType0,                                      \
    "Hook_TransparentDispatch_Type0_RenderBatch")                              \
  X(RenderPerfTransparentDispatchType1,                                        \
    "TransparentDispatch_Type1_ParticleEmitter",                               \
    rqTransparentDispatchType1, Hook_TransparentDispatchType1Timing,            \
    g_trampolineTransparentDispatchType1,                                      \
    "Hook_TransparentDispatch_Type1_ParticleEmitter")                          \
  X(RenderPerfTransparentDispatchType2,                                        \
    "TransparentDispatch_Type2_ImageLike", rqTransparentDispatchType2,          \
    Hook_TransparentDispatchType2Timing,                                       \
    g_trampolineTransparentDispatchType2,                                      \
    "Hook_TransparentDispatch_Type2_ImageLike")                                \
  X(RenderPerfTransparentDispatchType3,                                        \
    "TransparentDispatch_Type3_RibbonEmitter",                                 \
    rqTransparentDispatchType3, Hook_TransparentDispatchType3Timing,            \
    g_trampolineTransparentDispatchType3,                                      \
    "Hook_TransparentDispatch_Type3_RibbonEmitter")                            \
  X(RenderPerfTransparentDispatchType4,                                        \
    "TransparentDispatch_Type4_CallbackWrapper",                               \
    rqTransparentDispatchType4, Hook_TransparentDispatchType4Timing,            \
    g_trampolineTransparentDispatchType4,                                      \
    "Hook_TransparentDispatch_Type4_CallbackWrapper")

static bool War3RenderQueueDeepTimingHooksRuntimeEnabled() {
  // These are native queue-internal detours. Keep them out of production and
  // frame-level profiling until each entry has an isolated runtime gate.
  if constexpr (!dxvk::war3::internal::kNativePerfDetailHookTimingEnabled)
    return false;
  static const bool enabled = [] {
    const char* value =
        std::getenv("DXVK_WAR3_PERF_RENDERQUEUE_DEEP_HOOKS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

static bool War3WorldPrepareDeepTimingHooksRuntimeEnabled() {
  // These detours exist only to partition the native
  // WorldFrameUpdateAndPreparePasses body. Requiring both switches keeps the
  // normal and level-1 paths at exactly zero installation/runtime overhead.
  if constexpr (!dxvk::war3::internal::kNativePerfDetailHookTimingEnabled)
    return false;
  if (dxvk::war3::internal::War3PerfHookLevel() < 2)
    return false;
  static const bool enabled = [] {
    const char* value =
        std::getenv("DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

static bool War3WorldPrepareResidualTimingHooksRuntimeEnabled() {
  // This second diagnostic tier partitions three still-unattributed direct
  // callees without changing the established seven-hook deep probe. It is
  // installation-time opt-in so level 0/1 and ordinary level 2 remain byte-
  // for-byte on the original call path.
  if constexpr (!dxvk::war3::internal::kNativePerfDetailHookTimingEnabled)
    return false;
  if (dxvk::war3::internal::War3PerfHookLevel() < 2)
    return false;
  static const bool enabled = [] {
    const char* value =
        std::getenv("DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

static bool War3WorldPrepareCoreTimingHooksRuntimeEnabled() {
  // Third diagnostic tier for the still-unattributed Prepare native self.
  // Every target below has a mechanically proven 1.27a ABI. This remains an
  // installation-time opt-in because even sampled observers change the code
  // entry points whose cost they measure.
  if constexpr (!dxvk::war3::internal::kNativePerfDetailHookTimingEnabled)
    return false;
  if (dxvk::war3::internal::War3PerfHookLevel() < 2)
    return false;
  static const bool enabled = [] {
    const char* value =
        std::getenv("DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

static const char* WorldDispatchStageScopeName(int stage) {
  static constexpr std::array<const char*, 22> kStageScopes = {
      "Stage00", "Stage01", "Stage02", "Stage03", "Stage04", "Stage05",
      "Stage06", "Stage07", "Stage08", "Stage09", "Stage10", "Stage11",
      "Stage12", "Stage13", "Stage14", "Stage15", "Stage16", "Stage17",
      "Stage18", "Stage19", "Stage20", "Stage21"};
  if (stage >= 0 && stage < static_cast<int>(kStageScopes.size()))
    return kStageScopes[static_cast<size_t>(stage)];
  return "StageOther";
}

// 旧 per-draw std::string scope 已由固定 ID/QPC 的 War3HotHookCallTiming
// 完整替代。保留这个空 helper 让历史细分调用点不需要扰动业务控制流；
// 禁止重新启用，否则会与动态 Hook 树写入同一路径，造成总量/调用数双计，
// 并重新引入字符串、哈希和 TLS vector 的观察者开销。
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeRenderHookDrawScope(const char* /*name*/) {
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
    War3BatchTag tag = dxvk::war3::hooks::MapStageToTag(
        stage, dxvk::war3::internal::kNativeStageTagProfile);

    const bool gpuSkinUsesExactStage11Group =
        stage == 11 && dxvk::war3::gpu_skin::NativeBridgeHooksEnabled();
    if (gpuSkinUsesExactStage11Group ||
        dxvk::war3::hooks::ShouldSuppressStageTagByGroupMode(
            stage, dxvk::war3::internal::kNativeStageTagProfile,
            dxvk::war3::internal::kNativeTagWorldByGroupIdx)) {
      tag = War3BatchTag::Unknown;
    }

    // One upsert preserves an exact group tag already written by the nested
    // WorldObjects hook while still creating stage-only TerrainShadow entries.
    tracker.MarkTagStage(batchArray, before, after, tag, stage);
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
      auto scope = MakeRenderHookFrameScope("Hook_WorldDispatch/NativeSemanticPrepareStage");
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
        auto prepareScope = MakeRenderHookFrameScope("Hook_WorldDispatch/NativeSemanticFinalPrepareStage");
        const bool prepared =
            dxvk::war3::platform::DriveNativeShadowBackend(
                true, 32u);
        dxvk::war3::render::NoteNativeSemanticWorldStagePrepare(stage,
                                                                prepared);
        if (!prepared)
          return;
      }

      auto scope = MakeRenderHookFrameScope("Hook_WorldDispatch/NativeSemanticExecuteStage");
      const bool executed =
          dxvk::war3::platform::ExecuteNativeShadowBackendPreparedFrame();
      dxvk::war3::render::NoteNativeSemanticWorldStageExecute(stage,
                                                              executed);
    }
  }
}

static void TryWarVkLightningWorldStageDraw(int stage, int a5) {
  // Warcraft III submits native lightning in S20.  Execute after the native
  // WorldDispatch S20 call so WarVK ribbons share the same world ordering,
  // after S10 doodads and S11 world objects but before later overlays/UI.
  constexpr int kWarVkLightningStage = 20;
  if (stage != kWarVkLightningStage)
    return;
  if (a5 != 0)
    return;
  if (!dxvk::war3::War3Events::get().isJassReady())
    return;

  auto& lightning = dxvk::war3::render::War3LightningRuntime::instance();
  if (!lightning.hasActive())
    return;

  dxvk::war3::platform::TryBindNativeDeviceFromWar3Globals(
      "WarVKLightningStage20", false);

  auto scope = MakeRenderHookFrameScope("Hook_WorldDispatch/WarVKLightningS20");
  lightning.executePreparedFrame();
}

int __fastcall Hook_RenderDispatcher(int ctx1, int ctx2, int typeCode,
                                     int stage, int a5, int dataStore) {
  // 负责维护 dispatcher stage 栈，保证递归分发时阶段上下文不串台。
  auto perfScope = MakeRenderHookFrameScope("Hook_RenderDispatcher");
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
    auto origScope =
        MakeRenderHookFrameScope("Hook_RenderDispatcher/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_trampolineRenderDispatcher(ctx1, ctx2, typeCode, stage, a5,
                                          dataStore);
  }
  dxvk::war3::render::War3RenderDispatcher::instance().PopDispatcherStage(
      prevDispatcherStage);
  return result;
}

int __fastcall Hook_WorldFrameUpdateAndPreparePasses(void *thisPtr, void *edx,
                                                     int a2, int a3, int a4) {
  MarkWorldFrameThread();
  auto perfScope = MakeRenderHookFrameScope("Hook_WorldFramePrepare");

  if constexpr (!dxvk::war3::internal::kNativeWorldFrameBoundaryHooksEnabled) {
    if (g_trampolineWorldFrameUpdate) {
      auto nativeScope =
          MakeRenderHookFrameScope("Hook_WorldFramePrepare/NativeOriginal");
      War3HotHookBoundaryScope nativeHotHooks;
      return g_trampolineWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
    }
    if (g_originalWorldFrameUpdate) {
      auto nativeScope =
          MakeRenderHookFrameScope("Hook_WorldFramePrepare/NativeOriginal");
      War3HotHookBoundaryScope nativeHotHooks;
      return g_originalWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
    }
    return 0;
  }

  War3RenderState::OnWorldFramePrepareEnter();
  int result = 0;
  if (g_trampolineWorldFrameUpdate) {
    auto origScope =
        MakeRenderHookFrameScope("Hook_WorldFramePrepare/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_trampolineWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
  } else if (g_originalWorldFrameUpdate) {
    auto origScope =
        MakeRenderHookFrameScope("Hook_WorldFramePrepare/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_originalWorldFrameUpdate(thisPtr, edx, a2, a3, a4);
  }
  {
    auto afterScope =
        MakeRenderHookFrameScope("Hook_WorldFramePrepare/WarVKPostHook");
    War3RenderState::OnWorldFramePrepareExit();
  }
  return result;
}

int __fastcall Hook_WorldRenderScene(void *thisPtr, void *edx) {
  MarkWorldFrameThread();
  auto perfScope = MakeRenderHookFrameScope("Hook_WorldRenderScene");

  if constexpr (!dxvk::war3::internal::kNativeWorldFrameBoundaryHooksEnabled) {
    if (g_trampolineWorldRenderScene) {
      auto nativeScope =
          MakeRenderHookFrameScope("Hook_WorldRenderScene/NativeOriginal");
      War3HotHookBoundaryScope nativeHotHooks;
      return g_trampolineWorldRenderScene(thisPtr, edx);
    }
    if (g_originalWorldRenderScene) {
      auto nativeScope =
          MakeRenderHookFrameScope("Hook_WorldRenderScene/NativeOriginal");
      War3HotHookBoundaryScope nativeHotHooks;
      return g_originalWorldRenderScene(thisPtr, edx);
    }
    return 0;
  }

  War3RenderState::OnWorldRenderSceneEnter();
  int result = 0;
  if (g_trampolineWorldRenderScene) {
    auto origScope =
        MakeRenderHookFrameScope("Hook_WorldRenderScene/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_trampolineWorldRenderScene(thisPtr, edx);
  } else if (g_originalWorldRenderScene) {
    auto origScope =
        MakeRenderHookFrameScope("Hook_WorldRenderScene/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_originalWorldRenderScene(thisPtr, edx);
  }
  {
    auto afterScope =
        MakeRenderHookFrameScope("Hook_WorldRenderScene/WarVKPostHook");
    War3RenderState::OnWorldRenderSceneExit();
  }
  return result;
}

int __fastcall Hook_SceneSubmitBatch(void *thisPtr, void *edx, int a2, int a3,
                                     int stage, void *entries) {
  // SceneSubmitBatch 侧主要做阶段上下文桥接与同步诊断，不修改原提交语义。
  auto perfScope = MakeRenderHookFrameScope("Hook_SceneSubmitBatch");

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
    auto origScope =
        MakeRenderHookFrameScope("Hook_SceneSubmitBatch/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_trampolineSceneSubmitBatch(thisPtr, edx, a2, a3, stage, entries);
  } else if (g_originalSceneSubmitBatch) {
    auto origScope =
        MakeRenderHookFrameScope("Hook_SceneSubmitBatch/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
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
  auto perfScope = MakeRenderHookFrameScope("Hook_WorldDispatch");

  // FPS 覆盖仅需低频/幂等尝试；限制到 stage0 可避免在 WorldDispatch 热路径重复调用。
  if (stage == 0) {
    War3TryOverrideMaxFps();
  }

  using namespace dxvk::war3::internal;
  if (stage >= 0 && stage < 25) {
    static std::array<int8_t, 25u> s_effectiveStageEnabled = []() {
      std::array<int8_t, 25u> values = {};
      values.fill(-1);
      return values;
    }();
    const bool enabled = !kStageDebugEnabled || kStageDebug[stage];
    int8_t& previous = s_effectiveStageEnabled[size_t(stage)];
    if (previous < 0) {
      previous = enabled ? 1 : 0;
      if (!enabled) {
        dxvk::war3::render::PublishShadowStagePolicyTransition(
            static_cast<int16_t>(stage), false,
            dxvk::war3::render::VisibleRenderableRegistry::instance()
                .getFrameNumber());
      }
    } else if ((previous != 0) != enabled) {
      dxvk::war3::render::PublishShadowStagePolicyTransition(
          static_cast<int16_t>(stage), enabled,
          dxvk::war3::render::VisibleRenderableRegistry::instance()
              .getFrameNumber());
      previous = enabled ? 1 : 0;
    }
    if (unlikely(!enabled))
      return 0;
  }

  {
    auto timeSyncScope = MakeRenderHookFrameScope("Hook_WorldDispatch/TimeSync");
    if (stage == 0 || stage == 1) {
      // 说明：
      // - 优先直接调用原生 GetFloatGameState(GAME_STATE_TIME_OF_DAY)；
      // - War3RenderState::GameTime is a strict 0..24 TIME_OF_DAY contract.
      //   Do not publish elapsed seconds here: the shadow receiver treats any
      //   0..24 value as authoritative time-of-day, so a seconds fallback would
      //   compress a full day into 24 seconds and globally pulse shadows.
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
        float elapsedSeconds = -1.0f;
        if (runtimeReady) {
          const bool usedNative = TryFetchNativeTimeOfDay(gameTime);

          if (!usedNative) {
            if (dxvk::war3::War3Events::get().isGameStarted()) {
              if (!s_gameClockValid) {
                s_gameClockValid = true;
                s_gameClockStart = now;
              }

              elapsedSeconds = static_cast<float>(
                  std::chrono::duration_cast<std::chrono::duration<double>>(
                      now - s_gameClockStart)
                      .count());
              gameTime = -1.0f;
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

            elapsedSeconds = static_cast<float>(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - s_gameClockStart)
                    .count());
            gameTime = -1.0f;
          } else {
            s_gameClockValid = false;
            gameTime = -1.0f;
          }
        }

        if (gameTime >= 0.0f) {
          War3RenderState::SetGameTime(gameTime);
          War3VKBranding::TryShowBrandingMessage(gameTime);
          war3::ShaderManager::get().setGlobalFloat4(
              "Time", Vector4(gameTime, 0.0f, 0.0f, 0.0f));
        } else {
          War3RenderState::SetGameTime(-1.0f);
          if (dxvk::war3::War3Events::get().isGameStarted()) {
            War3VKBranding::TryShowBrandingMessage(elapsedSeconds);
            war3::ShaderManager::get().setGlobalFloat4(
                "Time", Vector4(elapsedSeconds, 0.0f, 0.0f, 0.0f));
          }
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
  if (a5 == 0) {
    War3RenderState::OnMainWorldStageEnter(stage);
  }

  int result = 0;
  {
    auto origScope =
        MakeRenderHookFrameScope("Hook_WorldDispatch/NativeOriginal");
    auto stageScope =
        MakeRenderHookFrameScope(WorldDispatchStageScopeName(stage));
    War3HotHookBoundaryScope nativeHotHooks;
    if (g_trampolineWorldDispatch) {
      result = g_trampolineWorldDispatch(thisPtr, edx, stage, a3, a4, a5);
    } else if (g_originalWorldDispatch) {
      result = g_originalWorldDispatch(thisPtr, edx, stage, a3, a4, a5);
    }
  }

  if (needBatchTracking) {
    auto trackScope =
        MakeRenderHookFrameScope("Hook_WorldDispatch/TrackRenderQueueUpdates");
    TrackRenderQueueUpdates(stage, before, batchArray);
  }

  TryNativeSemanticWorldStageValidation(stage, a3, a4, a5);
  TryWarVkLightningWorldStageDraw(stage, a5);

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
  auto perfScope = MakeRenderHookFrameScope("Hook_WorldObjects_RenderGroup");
  const bool phase1GroupCapture =
      dxvk::war3::render::IsWorldObjectsPhase1CaptureActive(groupIdx);
  const uint64_t pairedCaptureSequence =
      dxvk::war3::render::
          CurrentWorldObjectsPhase1PurePeriodicDispatchSequence();
  const bool pairedCapture = pairedCaptureSequence != 0u;
  const int64_t phase1GroupBegin = phase1GroupCapture || pairedCapture
      ? dxvk::high_resolution_clock::get_counter() : 0;
  if (pairedCapture) {
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        pairedCaptureSequence, 1u);
  }
  bool phase1GroupFinished = false;
  auto finishPhase1Group = [&]() {
    if (phase1GroupFinished || (!phase1GroupCapture && !pairedCapture))
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    const uint64_t ticks = end >= phase1GroupBegin
        ? uint64_t(end - phase1GroupBegin) : 0u;
    if (pairedCapture) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          pairedCaptureSequence, 1u);
      dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
          pairedCaptureSequence,
          dxvk::war3::render::
              WorldObjectsPhase1PairedTimingStage::WorldHookInclusive,
          ticks);
    }
    if (phase1GroupCapture) {
      dxvk::war3::render::RecordWorldObjectsPhase1HookInclusive(
          groupIdx, ticks);
    }
    phase1GroupFinished = true;
  };
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
        MakeRenderHookFrameScope("Hook_WorldObjects_RenderGroup/ShouldRenderCheck");
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

  if (!shouldRender) {
    finishPhase1Group();
    return 0;
  }

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
  const bool gpuSkinTracksExactStage11Group =
      groupIdx == 0 && dxvk::war3::gpu_skin::NativeBridgeHooksEnabled();
  bool shouldTrackGroupTag = gpuSkinTracksExactStage11Group;
  if constexpr (dxvk::war3::internal::kNativeTagWorldByGroupIdx) {
    shouldTrackGroupTag = shouldTrackGroupTag || (
        War3RenderState::IsBatchTagTrackingEnabled() &&
        g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
        g_numOfElementsPtr && g_batchArrayPtr);
  }
  shouldTrackGroupTag = shouldTrackGroupTag &&
      g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
      g_numOfElementsPtr && g_batchArrayPtr;
  if (shouldTrackGroupTag)
    beforeGroupTag = *g_numOfElementsPtr;

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
        MakeRenderHookFrameScope("Hook_WorldObjects_RenderGroup/CollectObjects");
    dxvk::war3::render::War3Renderer::instance().OnWorldObjectsGroup(thisPtr,
                                                                      groupIdx);
  }

  int result = 0;
  const int64_t pairedOriginalBegin = pairedCapture
      ? dxvk::high_resolution_clock::get_counter() : 0;
  if (pairedCapture) {
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        pairedCaptureSequence, 1u);
  }
  if (g_trampolineWorldObjectsRenderGroup) {
    auto origScope = MakeRenderHookFrameScope(
        "Hook_WorldObjects_RenderGroup/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_trampolineWorldObjectsRenderGroup(thisPtr, nullptr, groupIdx);
  } else if (g_originalWorldObjectsRenderGroup) {
    auto origScope = MakeRenderHookFrameScope(
        "Hook_WorldObjects_RenderGroup/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    result = g_originalWorldObjectsRenderGroup(thisPtr, nullptr, groupIdx);
  }
  if (pairedCapture) {
    const int64_t pairedOriginalEnd =
        dxvk::high_resolution_clock::get_counter();
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        pairedCaptureSequence, 1u);
    dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
        pairedCaptureSequence,
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::WorldOriginal,
        pairedOriginalEnd >= pairedOriginalBegin
            ? uint64_t(pairedOriginalEnd - pairedOriginalBegin) : 0u);
  }

  if (shouldTrackGroupTag) {
    const int64_t pairedTrackBegin = pairedCapture
        ? dxvk::high_resolution_clock::get_counter() : 0;
    if (pairedCapture) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          pairedCaptureSequence, 1u);
    }
    dxvk::war3::render::RenderQueueTracker::instance().TrackNewBatches(
        beforeGroupTag, groupIdx);
    if (pairedCapture) {
      const int64_t pairedTrackEnd =
          dxvk::high_resolution_clock::get_counter();
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          pairedCaptureSequence, 1u);
      dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
          pairedCaptureSequence,
          dxvk::war3::render::
              WorldObjectsPhase1PairedTimingStage::WorldTrackNewBatches,
          pairedTrackEnd >= pairedTrackBegin
              ? uint64_t(pairedTrackEnd - pairedTrackBegin) : 0u);
    }
  }

  groupTagScope.end();

  finishPhase1Group();

  return result;
}

int __fastcall Hook_ApplyDrawStateAndDraw(void* thisPtr, void* edx,
                                          int batchSlot) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ApplyDrawStateAndDraw, 4u);
  TryPublishPreparedSliceProbe(thisPtr);
  if (g_trampolineApplyDrawStateAndDraw) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return g_trampolineApplyDrawStateAndDraw(thisPtr, edx, batchSlot);
  }
  if (g_originalApplyDrawStateAndDraw) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return g_originalApplyDrawStateAndDraw(thisPtr, edx, batchSlot);
  }
  return 0;
}

// 已知原生队列内部节点的纯计时 detour：不改变参数、返回值或状态。
// 它们用于把 FlushAndReset/NativeOriginal 的残余拆成 StageUpdate 与透明队列，
// 地址和签名均来自现有 address book，不引入新的逆向依赖。
void __fastcall Hook_RenderQueueStageUpdateTiming(void* thisPtr,
                                                  void* /*edx*/) {
  // 实测可达数百次/帧；动态树与调用次数用 1/4 加权样本恢复，避免这个
  // 纯计时 detour 反过来显著放大 StageUpdate 本体。
  War3HotHookCallTiming hookTiming(
      War3HotHookId::RenderQueueStageUpdate, 4u);
  if (g_trampolineStageUpdateTiming) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineStageUpdateTiming(thisPtr);
  }
}

int __cdecl Hook_FlushTransparentTiming() {
  War3HotHookCallTiming hookTiming(War3HotHookId::FlushTransparent);
  if (g_trampolineFlushTransparentTiming) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return g_trampolineFlushTransparentTiming();
  }
  return 0;
}

// 原生透明队列在排序后按 entry type 进入五条固定分发路径。这里仅做
// trampoline 边界计时，不读取或改写 entry，不改变排序、剔除、状态绑定和 draw
// 语义。每条路径都挂在 FlushTransparent/NativeOriginalInclusive 下；其内部重新
// 进入的 CurrentDraw/D3D9 Draw/ShadowCapture 会继续成为该 Type 的真实子树。
// Type5 是队列条目携带的任意 callback，没有稳定目标地址，因此留在透明队列
// native residual 中，不用错误的统一签名强行 Hook。
void __fastcall Hook_TransparentDispatchType0Timing(uint32_t context,
                                                    void* batch) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::TransparentDispatchType0, 4u);
  if (g_trampolineTransparentDispatchType0) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTransparentDispatchType0(context, batch);
  }
}

void __fastcall Hook_TransparentDispatchType1Timing(void* batch) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::TransparentDispatchType1, 4u);
  if (g_trampolineTransparentDispatchType1) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTransparentDispatchType1(batch);
  }
}

void __fastcall Hook_TransparentDispatchType2Timing(void* batch) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::TransparentDispatchType2, 4u);
  if (g_trampolineTransparentDispatchType2) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTransparentDispatchType2(batch);
  }
}

void __fastcall Hook_TransparentDispatchType3Timing(void* batch) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::TransparentDispatchType3, 4u);
  if (g_trampolineTransparentDispatchType3) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTransparentDispatchType3(batch);
  }
}

void __fastcall Hook_TransparentDispatchType4Timing(void* batch) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::TransparentDispatchType4, 4u);
  if (g_trampolineTransparentDispatchType4) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTransparentDispatchType4(batch);
  }
}

// WorldFramePrepare 的第一层原生 callee 诊断。所有入口都由离线
// Game.dll 1.27a ASM 同时确认了 RVA、调用约定和实参位置；detour 只建立
// “整个 Hook / NativeOriginalInclusive”两层边界，不读取参数、不改状态。
// 它们只会在 PERF_LEVEL=2 且显式 env 打开时安装。
int __fastcall Hook_WorldPrepare_CameraBuildFrustum(
    void* viewMatrix, void* projectionMatrix, void* frustumPlanes) {
  auto hookScope =
      MakeRenderHookCpuScope("Hook_WorldPrepare_CameraBuildFrustum");
  if (!g_trampolineWorldPrepareCameraBuildFrustum)
    return 0;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_CameraBuildFrustum/NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  return g_trampolineWorldPrepareCameraBuildFrustum(
      viewMatrix, projectionMatrix, frustumPlanes);
}

int __fastcall Hook_WorldPrepare_TerrainShadowFlush(
    void* frustumPlanes, int planeCount) {
  auto hookScope =
      MakeRenderHookCpuScope("Hook_WorldPrepare_TerrainShadowFlush");
  if (!g_trampolineWorldPrepareTerrainShadowFlush)
    return 0;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_TerrainShadowFlush/NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  return g_trampolineWorldPrepareTerrainShadowFlush(
      frustumPlanes, planeCount);
}

int __fastcall Hook_WorldPrepare_TerrainExtraPass(
    void* cameraPosition, void* cameraDirection, float deltaSeconds) {
  auto hookScope =
      MakeRenderHookCpuScope("Hook_WorldPrepare_TerrainExtraPass");
  if (!g_trampolineWorldPrepareTerrainExtraPass)
    return 0;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_TerrainExtraPass/NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  return g_trampolineWorldPrepareTerrainExtraPass(
      cameraPosition, cameraDirection, deltaSeconds);
}

int __cdecl Hook_WorldPrepare_ShadowProjectorFlush() {
  auto hookScope =
      MakeRenderHookCpuScope("Hook_WorldPrepare_ShadowProjectorFlush");
  if (!g_trampolineWorldPrepareShadowProjectorFlush)
    return 0;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_ShadowProjectorFlush/NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  return g_trampolineWorldPrepareShadowProjectorFlush();
}

void __fastcall Hook_WorldPrepare_TargetIndicatorRingAdvance(
    void* worldFrame, void* /*edx*/, float deltaSeconds) {
  auto hookScope =
      MakeRenderHookCpuScope("Hook_WorldPrepare_TargetIndicatorRingAdvance");
  if (!g_trampolineWorldPrepareTargetIndicatorRingAdvance)
    return;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_TargetIndicatorRingAdvance/NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  g_trampolineWorldPrepareTargetIndicatorRingAdvance(worldFrame, deltaSeconds);
}

void __fastcall Hook_WorldPrepare_CinematicFilterTimeAdvance(
    void* clockState, void* /*edx*/, float deltaSeconds) {
  auto hookScope =
      MakeRenderHookCpuScope("Hook_WorldPrepare_CinematicFilterTimeAdvance");
  if (!g_trampolineWorldPrepareCinematicFilterTimeAdvance)
    return;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_CinematicFilterTimeAdvance/NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  g_trampolineWorldPrepareCinematicFilterTimeAdvance(clockState, deltaSeconds);
}

void __fastcall Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760(
    void* clockState, void* /*edx*/, float deltaSeconds) {
  auto hookScope =
      MakeRenderHookCpuScope(
          "Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760");
  if (!g_trampolineWorldPrepareRuntimeFlagClockAdvance3B8760)
    return;
  auto nativeScope = MakeRenderHookCpuScope(
      "Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760/"
      "NativeOriginalInclusive");
  War3HotHookBoundaryScope nativeHotHooks;
  g_trampolineWorldPrepareRuntimeFlagClockAdvance3B8760(
      clockState, deltaSeconds);
}

// Residual WorldFramePrepare diagnostics. The 1.27a caller/callee ASM proves:
//   0x368E00: ECX-only __thiscall, plain RET;
//   0x3AC130: ECX-only __thiscall, plain RET (ECX is an opaque frame token);
//   0x369370: ECX + float + pointer, RET 8.
// The hooks are pure observers. Keep their direct residual explicitly named
// ObserverOverhead instead of claiming it is WarVK business logic.
int __fastcall Hook_WorldPrepare_FlushDeferredSelectionObjects(
    void* worldFrame, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareFlushDeferredSelectionObjects, 8u);
  const auto trampoline = g_trampolineWorldPrepareFlushDeferredSelectionObjects;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldFrame);
}

int __fastcall Hook_WorldPrepare_GlobalRenderCallbackPass(
    void* frameContextToken, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareGlobalRenderCallbackPass, 8u);
  const auto trampoline = g_trampolineWorldPrepareGlobalRenderCallbackPass;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(frameContextToken);
}

int __fastcall Hook_WorldPrepare_RenderWaypointIndicators(
    void* worldFrame, void* /*edx*/, float deltaSeconds,
    const uint32_t* visibilityContext) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareRenderWaypointIndicators, 8u);
  const auto trampoline = g_trampolineWorldPrepareRenderWaypointIndicators;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldFrame, deltaSeconds, visibilityContext);
}

// Core WorldFramePrepare diagnostics. Unlike 0x368E90, these entries have
// ordinary ABIs confirmed from both sides of the call:
// - caller register/stack setup in 0x368480;
// - callee prologue register use;
// - callee RET/RET N stack cleanup.
// They are observer-only, sampled 1/8, and never read or modify game objects.
int __fastcall Hook_WorldPrepare_FrameUpdateGate(
    void* worldFrame, void* /*edx*/, float deltaSeconds,
    float frameDelta, void* frameContext) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareFrameUpdateGate, 8u);
  const auto trampoline = g_trampolineWorldPrepareFrameUpdateGate;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldFrame, deltaSeconds, frameDelta, frameContext);
}

int __fastcall Hook_WorldPrepare_GameUiFrameSync(
    void* gameUiFrameState, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareGameUiFrameSync, 8u);
  const auto trampoline = g_trampolineWorldPrepareGameUiFrameSync;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(gameUiFrameState);
}

int __fastcall Hook_WorldPrepare_UpdateIndicatorAnchor(
    void* worldFrame, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareUpdateIndicatorAnchor, 8u);
  const auto trampoline = g_trampolineWorldPrepareUpdateIndicatorAnchor;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldFrame);
}

int __fastcall Hook_WorldPrepare_CameraAdvance(
    void* cameraState, void* /*edx*/, float deltaSeconds,
    void* frameContext) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareCameraAdvance, 8u);
  const auto trampoline = g_trampolineWorldPrepareCameraAdvance;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(cameraState, deltaSeconds, frameContext);
}

int __fastcall Hook_WorldPrepare_CameraPrepareConstants(
    void* cameraConstants, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareCameraPrepareConstants, 8u);
  const auto trampoline = g_trampolineWorldPrepareCameraPrepareConstants;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(cameraConstants);
}

int __fastcall Hook_WorldPrepare_ViewProjPrepare(
    void* viewProjection, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareViewProjPrepare, 8u);
  const auto trampoline = g_trampolineWorldPrepareViewProjPrepare;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(viewProjection);
}

int __cdecl Hook_WorldPrepare_SceneQueryFlushSync() {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareSceneQueryFlushSync, 8u);
  const auto trampoline = g_trampolineWorldPrepareSceneQueryFlushSync;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline();
}

int __fastcall Hook_WorldPrepare_FixedPointRemap(
    void* output, void* input, const void* scale) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareFixedPointRemap, 8u);
  const auto trampoline = g_trampolineWorldPrepareFixedPointRemap;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(output, input, scale);
}

int __cdecl Hook_WorldPrepare_PostVisibilityGlobalAdvanceA() {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPreparePostVisibilityGlobalAdvanceA, 8u);
  const auto trampoline =
      g_trampolineWorldPreparePostVisibilityGlobalAdvanceA;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline();
}

// ABI proof for Game.dll 1.27a RVA 0x377FD0:
// 1) 0x3784C8 loads the scanned entry in ECX and calls the no-stack-argument
//    bridge at 0x6374A0.
// 2) That bridge performs only `mov ecx,[ecx+50h]`, a null test, and a tail
//    jump to 0x377FD0, without changing ESP or manufacturing stack arguments.
// 3) 0x377FD0 saves ECX as its object pointer and returns with a plain RET.
// Therefore the inner boundary is an ordinary one-argument __thiscall.  We
// hook the long inner body rather than the 12-byte tail bridge so the parent
// residual remains the array scan/handle validation and MinHook does not need
// to relocate the short conditional tail jump.
void __fastcall Hook_WorldPrepare_PostVisibility_FrameAnchorUpdate(
    void* frameAnchorState, void* /*edx*/) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPreparePostVisibilityFrameAnchorUpdate, 8u);
  const auto trampoline =
      g_trampolineWorldPreparePostVisibilityFrameAnchorUpdate;
  if (!trampoline)
    return;
  War3HotHookNativeScope nativeTiming(hookTiming);
  trampoline(frameAnchorState);
}

// ABI proof for RVA 0x358CF0:
// - The sole exported call site (0x378148..0x378156) places the world object in
//   ECX, the 2-float output in EDX, and pushes exactly one additional output
//   pointer.
// - The callee copies ECX/EDX before reading [EBP+8], then both exits use
//   `retn 4`.  This is a three-argument __fastcall, with no implicit live
//   register dependency.
int __fastcall Hook_WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery(
    void* worldObject, void* projectedXY, void* projectedZ) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPreparePostVisibilityFrameAnchorVisibilityQuery,
      8u);
  const auto trampoline =
      g_trampolineWorldPreparePostVisibilityFrameAnchorVisibilityQuery;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldObject, projectedXY, projectedZ);
}

int __cdecl Hook_WorldPrepare_PostVisibilityGlobalAdvanceB() {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPreparePostVisibilityGlobalAdvanceB, 8u);
  const auto trampoline =
      g_trampolineWorldPreparePostVisibilityGlobalAdvanceB;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline();
}

int __fastcall Hook_WorldPrepare_VisibilityTailAdvanceA(
    void* worldFrame, void* /*edx*/, int arg0, int arg1,
    float deltaSeconds) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareVisibilityTailAdvanceA, 8u);
  const auto trampoline = g_trampolineWorldPrepareVisibilityTailAdvanceA;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldFrame, arg0, arg1, deltaSeconds);
}

int __fastcall Hook_WorldPrepare_VisibilityTailAdvanceB(
    void* worldFrame, void* /*edx*/, int arg0, int arg1,
    float deltaSeconds) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldPrepareVisibilityTailAdvanceB, 8u);
  const auto trampoline = g_trampolineWorldPrepareVisibilityTailAdvanceB;
  if (!trampoline)
    return 0;
  War3HotHookNativeScope nativeTiming(hookTiming);
  return trampoline(worldFrame, arg0, arg1, deltaSeconds);
}

int __fastcall Hook_ApplyDrawStateAndSamplerPair(void* geosetData,
                                                  void* layerDispatch,
                                                  void* layerState) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ApplyDrawStateAndSamplerPair, 4u);
  const auto callNativeOriginal = [&]() {
    const ApplyDrawStateAndSamplerPairFn trampoline =
        g_trampolineApplyDrawStateAndSamplerPair;
    War3HotHookNativeScope nativeTiming(hookTiming);
    return trampoline != nullptr
        ? trampoline(geosetData, layerDispatch, layerState) : 0;
  };
  if (dxvk::war3::gpu_skin::
          NativeGpuSkinB1BorrowedFlushPinPassThroughFast()) {
    return callNativeOriginal();
  }
  // B1 的精确负候选 dispatch 不会消费 geoset/layer 语义；跳过整个
  // SemanticFrame/生产计时/DetourPin 链，候选和不确定状态仍走原路径。
  if (!dxvk::war3::gpu_skin::NativeGpuSkinSemanticObservationRequired()) {
    return callNativeOriginal();
  }
  const bool productionTimingSampled = SampleNextNativeProductionOrdinal(
      t_nativeProductionTiming.semanticOrdinal);
  const int64_t productionInclusiveStart = productionTimingSampled
      ? dxvk::high_resolution_clock::get_counter() : 0;
  uint64_t productionOriginalTicks = 0u;
  int result = 0;
  {
    NativeGpuSkinDetourPin detourPin(NativeGpuSkinDetourKind::Apply);
    uintptr_t callerAddress = 0u;
#if defined(_MSC_VER)
    callerAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__)
    callerAddress =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif

    dxvk::war3::gpu_skin::NativeSemanticScope semanticScope(
        reinterpret_cast<uintptr_t>(geosetData),
        reinterpret_cast<uintptr_t>(layerDispatch),
        reinterpret_cast<uintptr_t>(layerState), callerAddress);
    const int64_t productionOriginalStart = productionTimingSampled
        ? dxvk::high_resolution_clock::get_counter() : 0;
    result = callNativeOriginal();
    if (productionTimingSampled) {
      const int64_t productionOriginalEnd =
          dxvk::high_resolution_clock::get_counter();
      productionOriginalTicks = NativeProductionElapsedTicks(
          productionOriginalStart, productionOriginalEnd);
    }
  }
  if (productionTimingSampled) {
    // Both endpoints precede the one writer generation. Inclusive covers the
    // detour pin and semantic scope lifetime; Original covers the exact same
    // physical Apply call's trampoline interval.
    const int64_t productionInclusiveEnd =
        dxvk::high_resolution_clock::get_counter();
    const std::array<
        dxvk::war3::gpu_skin::NativeProductionTimingEntry, 2u> entries = {{
      {dxvk::war3::gpu_skin::NativeProductionTimingStage::SemanticInclusive,
       NativeProductionElapsedTicks(
           productionInclusiveStart, productionInclusiveEnd)},
      {dxvk::war3::gpu_skin::NativeProductionTimingStage::SemanticOriginal,
       productionOriginalTicks},
    }};
    dxvk::war3::gpu_skin::RecordNativeProductionTimingBatch(
        entries.data(), entries.size());
  }
  return result;
}

int __fastcall Hook_GxDeviceD3dDynamicVertexUpload(
    void* gxDeviceD3d, void* /*edx*/, uint32_t vertexCount,
    const void* positions, uint32_t positionStride, const void* normals,
    uint32_t normalStride, const void* extra, uint32_t extraStride,
    const void* groupSlots, uint32_t groupSlotStride, const void* uv0,
    uint32_t uv0Stride, const void* uv1, uint32_t uv1Stride) {
  War3HotHookCompletionTiming hookTiming(
      War3HotHookId::GpuSkinDynamicVertexUpload, 4u);
  const auto callNativeOriginal = [&]() {
    const GxDeviceD3dDynamicVertexUploadFn trampoline =
        g_trampolineGxDeviceD3dDynamicVertexUpload;
    War3HotHookCompletionNativeScope nativeTiming(hookTiming);
    return trampoline != nullptr
        ? trampoline(gxDeviceD3d, vertexCount, positions, positionStride,
                     normals, normalStride, extra, extraStride, groupSlots,
                     groupSlotStride, uv0, uv0Stride, uv1, uv1Stride)
        : 0;
  };
  if (dxvk::war3::gpu_skin::
          NativeGpuSkinB1BorrowedFlushPinPassThroughFast()) {
    return callNativeOriginal();
  }
  NativeGpuSkinDetourPin detourPin(
      NativeGpuSkinDetourKind::OuterUpload);

  // Disabled 模式以及 VS-B1 已确认的非候选 dispatch 不需要进入全局
  // upload/poison 状态机；原生函数本身仍完整执行。
  if (!dxvk::war3::gpu_skin::NativeBridgeHooksEnabled() ||
      dxvk::war3::gpu_skin::NativeGpuSkinB1UnobservedNativePathSafe()) {
    return callNativeOriginal();
  }

  // The overwhelmingly common outside/no-poison cohort needs no manager,
  // resource or O1 decision. Probe it before even constructing the sampled
  // production timing scope; the bridge TLS receipt can only force the exact
  // original outer upload and its nested original CPU kernel.
  class OutsideNoPoisonDirectOriginalGuard final {
  public:
    explicit OutsideNoPoisonDirectOriginalGuard(
        uintptr_t gxDeviceD3d, uint32_t vertexCount,
        bool trampolineAvailable) noexcept
        : m_cookie(trampolineAvailable ? dxvk::war3::gpu_skin::
              TryBeginNativeOutsideNoPoisonDirectOriginal(
                  gxDeviceD3d, vertexCount) : 0u) {
    }
    ~OutsideNoPoisonDirectOriginalGuard() noexcept {
      if (m_cookie != 0u) {
        dxvk::war3::gpu_skin::
            CancelNativeOutsideNoPoisonDirectOriginal(m_cookie);
      }
    }
    bool active() const noexcept { return m_cookie != 0u; }
    void complete() noexcept {
      if (m_cookie == 0u)
        return;
      dxvk::war3::gpu_skin::
          CompleteNativeOutsideNoPoisonDirectOriginal(m_cookie);
      m_cookie = 0u;
    }

  private:
    uint64_t m_cookie = 0u;
  };

  const GxDeviceD3dDynamicVertexUploadFn directOriginalTrampoline =
      g_trampolineGxDeviceD3dDynamicVertexUpload;
  OutsideNoPoisonDirectOriginalGuard directOriginal(
      reinterpret_cast<uintptr_t>(gxDeviceD3d), vertexCount,
      directOriginalTrampoline != nullptr);

  if (directOriginal.active()) {
    const int result = callNativeOriginal();
    directOriginal.complete();
    return result;
  }

  const bool timingEnabled =
      dxvk::war3::gpu_skin::NativeBridgeFullDiagnosticsEnabled();
  const int64_t timingStart = timingEnabled
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  NativeProductionOuterTimingScope productionTiming;

  const bool canonicalGpuSkinSources =
      positions != nullptr && normals != nullptr && groupSlots != nullptr &&
      (uv1 == nullptr || uv0 != nullptr);
  const uint64_t gpuSkinOutputStride = canonicalGpuSkinSources
      ? 24u + (extra != nullptr ? 4u : 0u) +
          (uv0 != nullptr ? 8u : 0u) + (uv1 != nullptr ? 8u : 0u)
      : 0u;
  const uint64_t gpuSkinOutputBytes =
      uint64_t(vertexCount) * gpuSkinOutputStride;

  class CpuOnlyUploadFastPathGuard final {
  public:
    explicit CpuOnlyUploadFastPathGuard(
        NativeProductionOuterTimingScope& timing,
        uintptr_t gxDeviceD3d, uint32_t vertexCount,
        uint64_t outputByteCount) noexcept
        : m_timing(timing) {
      const int64_t timingStart = m_timing.BeginStage();
      m_cookie = dxvk::war3::gpu_skin::
          TryBeginNativeCpuOnlyUploadFastPath(
              gxDeviceD3d, vertexCount, outputByteCount,
              &m_populationToken, m_timing.AdmissionProbe(),
              &m_dispatchSeal, &m_shadowCookie);
      if (m_dispatchSeal)
        m_timing.EndDispatchSealAdmission(timingStart);
      else
        m_timing.EndAdmission(timingStart, m_cookie != 0u);
    }
    ~CpuOnlyUploadFastPathGuard() noexcept {
      if (m_shadowCookie != 0u) {
        dxvk::war3::gpu_skin::CancelNativeOutsidePoisonShadow(
            m_shadowCookie);
        m_shadowCookie = 0u;
      }
      if (m_cookie != 0u) {
        const int64_t timingStart = m_timing.BeginStage();
        dxvk::war3::gpu_skin::CancelNativeCpuOnlyUploadFastPath(m_cookie);
        m_timing.EndStage(
            m_dispatchSeal
                ? dxvk::war3::gpu_skin::
                      NativeProductionTimingStage::OuterDispatchSealCancel
                : dxvk::war3::gpu_skin::
                      NativeProductionTimingStage::OuterFastCancel,
            timingStart);
      }
    }
    bool active() const noexcept { return m_cookie != 0u; }
    bool dispatchSealed() const noexcept { return m_dispatchSeal; }
    const dxvk::war3::gpu_skin::NativeOutsideUploadPopulationToken&
    populationToken() const noexcept {
      return m_populationToken;
    }
    void settleShadow(int32_t originalResult) noexcept {
      if (m_shadowCookie == 0u)
        return;
      dxvk::war3::gpu_skin::SettleNativeOutsidePoisonShadow(
          m_shadowCookie, originalResult);
      m_shadowCookie = 0u;
    }
    void complete(int32_t originalResult) noexcept {
      if (m_cookie == 0u)
        return;
      // Shadow settlement must precede fast completion: the latter may
      // process and complete a pending bridge reset.
      settleShadow(originalResult);
      const int64_t timingStart = m_timing.BeginStage();
      dxvk::war3::gpu_skin::CompleteNativeCpuOnlyUploadFastPath(m_cookie);
      m_timing.EndStage(
          m_dispatchSeal
              ? dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterDispatchSealComplete
              : dxvk::war3::gpu_skin::
                    NativeProductionTimingStage::OuterFastComplete,
          timingStart);
      m_cookie = 0u;
    }

  private:
    NativeProductionOuterTimingScope& m_timing;
    uint64_t m_cookie = 0u;
    uint64_t m_shadowCookie = 0u;
    bool m_dispatchSeal = false;
    dxvk::war3::gpu_skin::NativeOutsideUploadPopulationToken
        m_populationToken;
  } cpuOnlyFastPath(
      productionTiming, reinterpret_cast<uintptr_t>(gxDeviceD3d),
      vertexCount, gpuSkinOutputBytes);

  if (cpuOnlyFastPath.active()) {
    int result = 0;
    const int64_t bodyTimingStart = productionTiming.BeginStage();
    result = callNativeOriginal();
    productionTiming.EndStage(
        cpuOnlyFastPath.dispatchSealed()
            ? dxvk::war3::gpu_skin::
                  NativeProductionTimingStage::OuterDispatchSealBody
            : dxvk::war3::gpu_skin::
                  NativeProductionTimingStage::OuterFastBody,
        bodyTimingStart);
    cpuOnlyFastPath.complete(result);
    return result;
  }

  // Admission rejected: remain on the exact ASM-bound generic root. The
  // enclosing fallback-inclusive sample began after sampled admission
  // bookkeeping and ends only after the detour pin leaves. These substages
  // intentionally use recovery inequalities (each <= inclusive), since the
  // caller-owned SEH or a missing trampoline may prevent an individual
  // normal-return endpoint.
  const int64_t fallbackBeginTimingStart = productionTiming.BeginStage();
  dxvk::war3::gpu_skin::NativeUploadCall call = {};
  call.gxDeviceD3d = reinterpret_cast<uintptr_t>(gxDeviceD3d);
  call.vertexCount = vertexCount;
  call.positions = reinterpret_cast<uintptr_t>(positions);
  call.positionStride = positionStride;
  call.normals = reinterpret_cast<uintptr_t>(normals);
  call.normalStride = normalStride;
  call.extra = reinterpret_cast<uintptr_t>(extra);
  call.extraStride = extraStride;
  call.groupSlots = reinterpret_cast<uintptr_t>(groupSlots);
  call.groupSlotStride = groupSlotStride;
  call.uv0 = reinterpret_cast<uintptr_t>(uv0);
  call.uv0Stride = uv0Stride;
  call.uv1 = reinterpret_cast<uintptr_t>(uv1);
  call.uv1Stride = uv1Stride;
  call.outsideAdmissionPopulation = cpuOnlyFastPath.populationToken();

  auto observation = dxvk::war3::gpu_skin::BeginNativeUpload(call);
  productionTiming.EndStage(
      dxvk::war3::gpu_skin::
          NativeProductionTimingStage::OuterFallbackBegin,
      fallbackBeginTimingStart);
  int result = 0;
  {
    dxvk::war3::gpu_skin::NativeUploadInFlightScope inFlight(observation);
    // The complete +0x68 upload always executes exactly once. Its nested
    // 0x6F0EDDC0 call is the only legal CPU-write bypass point. IDA ASM shows
    // +0x68 returns at 0x6F0EEC17, before the separate +0x6C index upload and
    // either 0x6F0E352B or 0x6F13A6BE primitive-batch flush.
    if (g_trampolineGxDeviceD3dDynamicVertexUpload != nullptr) {
      const int64_t fallbackBodyTimingStart =
          productionTiming.BeginStage();
      result = callNativeOriginal();
      productionTiming.EndStage(
          dxvk::war3::gpu_skin::
              NativeProductionTimingStage::OuterFallbackBody,
          fallbackBodyTimingStart);
    }
    // Publish while the private in-flight index snapshot is still owned by
    // this transaction; PublishCompletedUpload moves it into active dispatch.
    const int64_t fallbackCompleteTimingStart =
        productionTiming.BeginStage();
    dxvk::war3::gpu_skin::CompleteNativeUpload(observation, result);
    productionTiming.EndStage(
        dxvk::war3::gpu_skin::
            NativeProductionTimingStage::OuterFallbackComplete,
        fallbackCompleteTimingStart);
    cpuOnlyFastPath.settleShadow(result);
  }
  const int64_t timingElapsed = timingEnabled
      ? dxvk::high_resolution_clock::get_counter() - timingStart
      : 0;
  if (timingEnabled && timingElapsed > 0) {
    dxvk::war3::gpu_skin::RecordNativeOuterUploadTiming(
        uint64_t(timingElapsed));
  }
  return result;
}

void __fastcall Hook_GxDeviceD3dSkinCopyKernel(
    void* gxDeviceD3d, void* /*edx*/, void* mappedDst) {
  // 原生调用方在 trampoline 外拥有 SEH；completion 版本只在正常返回后
  // 提交本地样本，不会让一次 native fault 留下未闭合的 TLS 调用栈。
  War3HotHookCompletionTiming hookTiming(
      War3HotHookId::GpuSkinCopyKernel, 4u);
  const auto callNativeOriginal = [&]() {
    const GxDeviceD3dSkinCopyKernelFn trampoline =
        g_trampolineGxDeviceD3dSkinCopyKernel;
    if (trampoline == nullptr)
      return false;
    War3HotHookCompletionNativeScope nativeTiming(hookTiming);
    trampoline(gxDeviceD3d, mappedDst);
    return true;
  };
  if (dxvk::war3::gpu_skin::
          NativeGpuSkinB1BorrowedFlushPinPassThroughFast()) {
    callNativeOriginal();
    return;
  }
  NativeGpuSkinDetourPin detourPin(NativeGpuSkinDetourKind::Kernel);
  const GxDeviceD3dSkinCopyKernelFn directOriginalTrampoline =
      g_trampolineGxDeviceD3dSkinCopyKernel;
  if (dxvk::war3::gpu_skin::
          TryRouteNativeOutsideNoPoisonDirectOriginalKernel(
              reinterpret_cast<uintptr_t>(gxDeviceD3d), mappedDst,
              directOriginalTrampoline != nullptr)) {
    if (directOriginalTrampoline == nullptr)
      return;
    callNativeOriginal();
    // The native caller owns SEH around the original kernel. A fault unwinds
    // past this endpoint, so only an actual normal return can close it.
    dxvk::war3::gpu_skin::
        NotifyNativeOutsideNoPoisonDirectOriginalReturned(
            reinterpret_cast<uintptr_t>(gxDeviceD3d), mappedDst);
    return;
  }

  // VS-B1 负候选的 CPU kernel 不需要进入完整 detour 状态机；只有已发布
  // input lease 的 candidate 或遗留 poison 才能关闭这个原生直通门。
  if (!dxvk::war3::gpu_skin::NativeBridgeHooksEnabled() ||
      dxvk::war3::gpu_skin::NativeGpuSkinB1UnobservedNativePathSafe()) {
    callNativeOriginal();
    return;
  }

  NativeProductionKernelTimingScope productionTiming;
  const int64_t evaluateTimingStart = productionTiming.BeginStage();
  const auto outcome =
      dxvk::war3::gpu_skin::EvaluateNativeSkinKernelDetour(
          reinterpret_cast<uintptr_t>(gxDeviceD3d), mappedDst);
  productionTiming.EndStage(
      dxvk::war3::gpu_skin::
          NativeProductionTimingStage::KernelEvaluate,
      evaluateTimingStart);
  using dxvk::war3::gpu_skin::NativeKernelDetourOutcome;
  const bool callsTrampoline =
      outcome == NativeKernelDetourOutcome::CallOriginal ||
      outcome == NativeKernelDetourOutcome::CallOriginalNoNotify;
  if (!callsTrampoline) {
    return;
  }
  const bool notifyNormalReturn =
      outcome == NativeKernelDetourOutcome::CallOriginal;

  // The two CallOriginal outcomes are the only outcomes allowed to enter the
  // trampoline. In particular, mappedDst==NULL always reaches the original so
  // caller-owned native SEH observes the same write fault as unhooked Game.dll.
  if (g_trampolineGxDeviceD3dSkinCopyKernel == nullptr)
    return;
  const bool timingEnabled =
      dxvk::war3::gpu_skin::NativeBridgeFullDiagnosticsEnabled();
  const int64_t timingStart = timingEnabled
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  const int64_t originalTimingStart = productionTiming.BeginStage();
  callNativeOriginal();
  productionTiming.EndStage(
      dxvk::war3::gpu_skin::
          NativeProductionTimingStage::KernelOriginal,
      originalTimingStart);
  const int64_t timingElapsed = timingEnabled
      ? dxvk::high_resolution_clock::get_counter() - timingStart
      : 0;
  if (timingEnabled && timingElapsed > 0) {
    dxvk::war3::gpu_skin::RecordNativeOriginalKernelTiming(
        uint64_t(timingElapsed));
  }
  // 0x6F0EEB7B..8A is caller-owned SEH. A fault in the trampoline unwinds past
  // this instruction to 0x6F0EEB99, so only a real normal return reaches it.
  // Exact CPU-only routes have no generic in-flight observation, but the
  // separate CPU-only endpoint still freezes dispatch-seal and outside-poison
  // proofs before the enclosing Unlock/outer settlement.
  if (notifyNormalReturn) {
    const int64_t notifyTimingStart = productionTiming.BeginStage();
    dxvk::war3::gpu_skin::NotifyNativeSkinKernelOriginalReturned(
        reinterpret_cast<uintptr_t>(gxDeviceD3d), mappedDst);
    productionTiming.EndStage(
        dxvk::war3::gpu_skin::
            NativeProductionTimingStage::KernelNotify,
        notifyTimingStart);
  } else {
    // Manager-free dispatch seal and production O1 both need an exact
    // normal-return endpoint distinct from Unlock and outer completion.
    dxvk::war3::gpu_skin::
        NotifyNativeCpuOnlySkinKernelOriginalReturned(
            reinterpret_cast<uintptr_t>(gxDeviceD3d), mappedDst);
  }
}

struct NativeGpuSkinDispatchSemantic {
  int32_t stage = -1;
  int32_t batchTag = static_cast<int32_t>(War3BatchTag::Unknown);
  bool forceFailClosed = false;
};

// This snapshot is GPU-skin-only. Compatibility render tag/stage locals keep
// their existing GetTagStage flow and must never seed this eligibility scope.
static NativeGpuSkinDispatchSemantic ResolveNativeGpuSkinDispatchSemantic(
    void* renderablePart, uint32_t liveLayerIndex) {
  NativeGpuSkinDispatchSemantic result = {};
  if (!dxvk::war3::gpu_skin::NativeBridgeHooksEnabled())
    return result;
  const bool productionTimingSampled = SampleNextNativeProductionOrdinal(
      t_nativeProductionTiming.dispatchSemanticOrdinal);
  const int64_t productionTimingStart = productionTimingSampled
      ? dxvk::high_resolution_clock::get_counter() : 0;

  uint32_t failureMask =
      dxvk::war3::gpu_skin::NativeDispatchSemanticFailureNone;
  dxvk::war3::render::RenderQueueSemanticState semantic = {};
  if (!dxvk::war3::render::RenderQueueTracker::instance().GetSemanticState(
          renderablePart, semantic)) {
    failureMask |=
        dxvk::war3::gpu_skin::NativeDispatchSemanticFailureQueryMiss;
  } else {
    result.stage = semantic.stage;
    result.batchTag = static_cast<int32_t>(semantic.tag);
    if (semantic.HasConflict()) {
      failureMask |=
          dxvk::war3::gpu_skin::NativeDispatchSemanticFailureConflict;
    }
    if (semantic.tag == War3BatchTag::Unknown || semantic.stage < 0 ||
        !semantic.HasKnownLayer()) {
      failureMask |=
          dxvk::war3::gpu_skin::NativeDispatchSemanticFailureUnknown;
    }
    if (semantic.HasKnownLayer() &&
        semantic.layerIndex != liveLayerIndex) {
      failureMask |=
          dxvk::war3::gpu_skin::NativeDispatchSemanticFailureLayerMismatch;
    }
  }

  result.forceFailClosed =
      failureMask !=
      dxvk::war3::gpu_skin::NativeDispatchSemanticFailureNone;
  if (result.forceFailClosed) {
    dxvk::war3::gpu_skin::ReportNativeDispatchSemanticFailures(failureMask);
  }
  if (productionTimingSampled) {
    const int64_t productionTimingEnd =
        dxvk::high_resolution_clock::get_counter();
    dxvk::war3::gpu_skin::RecordNativeProductionTiming(
        dxvk::war3::gpu_skin::
            NativeProductionTimingStage::DispatchSemanticLookup,
        NativeProductionElapsedTicks(
            productionTimingStart, productionTimingEnd));
  }
  return result;
}

class WorldObjectsPhase1PeriodicDispatchScope {
public:
  explicit WorldObjectsPhase1PeriodicDispatchScope(bool special) noexcept
      : m_special(special),
        m_eventSequence(dxvk::war3::render::
            CurrentWorldObjectsPhase1PurePeriodicDispatchSequence()),
        m_active(m_eventSequence != 0u),
        m_begin(m_active ? dxvk::high_resolution_clock::get_counter() : 0) {
    if (m_active) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
    }
  }

  ~WorldObjectsPhase1PeriodicDispatchScope() {
    if (!m_active)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        m_eventSequence, 1u);
    dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
        m_eventSequence,
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchRoot,
        end >= m_begin ? uint64_t(end - m_begin) : 0u);
    dxvk::war3::render::RecordWorldObjectsPhase1PeriodicDispatch(
        m_eventSequence, m_special, m_group0Stage,
        m_worldFastEligibleIgnoringIdentity,
        m_worldFastBlockedByIdentity);
  }

  void SetStageTag(War3BatchTag tag) noexcept {
    if (m_active)
      m_group0Stage = tag == War3BatchTag::WorldObjects;
  }

  void SetWorldFastEligibility(bool eligibleIgnoringIdentity,
                               bool blockedByIdentity) noexcept {
    if (!m_active)
      return;
    m_worldFastEligibleIgnoringIdentity = eligibleIgnoringIdentity;
    m_worldFastBlockedByIdentity =
        eligibleIgnoringIdentity && blockedByIdentity;
  }

  WorldObjectsPhase1PeriodicDispatchScope(
      const WorldObjectsPhase1PeriodicDispatchScope&) = delete;
  WorldObjectsPhase1PeriodicDispatchScope& operator=(
      const WorldObjectsPhase1PeriodicDispatchScope&) = delete;

private:
  bool m_special = false;
  uint64_t m_eventSequence = 0u;
  bool m_active = false;
  bool m_group0Stage = false;
  bool m_worldFastEligibleIgnoringIdentity = false;
  bool m_worldFastBlockedByIdentity = false;
  int64_t m_begin = 0;
};

class WorldObjectsPhase1PairedTimingScope {
public:
  explicit WorldObjectsPhase1PairedTimingScope(
      dxvk::war3::render::WorldObjectsPhase1PairedTimingStage stage) noexcept
      : m_eventSequence(dxvk::war3::render::
            CurrentWorldObjectsPhase1PurePeriodicDispatchSequence()),
        m_stage(stage),
        m_begin(m_eventSequence != 0u
            ? dxvk::high_resolution_clock::get_counter() : 0) {
    if (m_eventSequence != 0u) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
    }
  }

  ~WorldObjectsPhase1PairedTimingScope() noexcept { Stop(); }

  void Stop() noexcept {
    if (m_eventSequence == 0u || m_stopped)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        m_eventSequence, 1u);
    dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
        m_eventSequence, m_stage,
        end >= m_begin ? uint64_t(end - m_begin) : 0u);
    m_stopped = true;
  }

  WorldObjectsPhase1PairedTimingScope(
      const WorldObjectsPhase1PairedTimingScope&) = delete;
  WorldObjectsPhase1PairedTimingScope& operator=(
      const WorldObjectsPhase1PairedTimingScope&) = delete;

private:
  uint64_t m_eventSequence = 0u;
  dxvk::war3::render::WorldObjectsPhase1PairedTimingStage m_stage;
  int64_t m_begin = 0;
  bool m_stopped = false;
};

class WorldObjectsPhase1NativeEndBracket {
public:
  WorldObjectsPhase1NativeEndBracket() noexcept
      : m_eventSequence(dxvk::war3::render::
            CurrentWorldObjectsPhase1PurePeriodicDispatchSequence()) {
  }

  ~WorldObjectsPhase1NativeEndBracket() noexcept {
    if (m_eventSequence == 0u || !m_armed)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        m_eventSequence, 1u);
    dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
        m_eventSequence,
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchNativeEnd,
        end >= m_begin ? uint64_t(end - m_begin) : 0u);
  }

  void Arm() noexcept {
    if (m_eventSequence == 0u || m_armed)
      return;
    m_begin = dxvk::high_resolution_clock::get_counter();
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        m_eventSequence, 1u);
    m_armed = true;
  }

private:
  uint64_t m_eventSequence = 0u;
  int64_t m_begin = 0;
  bool m_armed = false;
};

class WorldObjectsPhase1NativeEndArm {
public:
  explicit WorldObjectsPhase1NativeEndArm(
      WorldObjectsPhase1NativeEndBracket& bracket) noexcept
      : m_bracket(bracket) {
  }
  ~WorldObjectsPhase1NativeEndArm() noexcept { m_bracket.Arm(); }

private:
  WorldObjectsPhase1NativeEndBracket& m_bracket;
};

class WorldObjectsPhase1PeriodicDispatchRootScope {
public:
  WorldObjectsPhase1PeriodicDispatchRootScope() noexcept
      : m_eventSequence(dxvk::war3::render::
            CurrentWorldObjectsPhase1PurePeriodicDispatchSequence()),
        m_begin(m_eventSequence != 0u
            ? dxvk::high_resolution_clock::get_counter()
            : 0) {
    if (m_eventSequence != 0u) {
      dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
          m_eventSequence, 1u);
    }
  }

  ~WorldObjectsPhase1PeriodicDispatchRootScope() {
    if (m_eventSequence == 0u)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    dxvk::war3::render::RecordWorldObjectsPhase1PairedQpcReads(
        m_eventSequence, 1u);
    const uint64_t ticks =
        end >= m_begin ? uint64_t(end - m_begin) : 0u;
    dxvk::war3::render::RecordWorldObjectsPhase1PeriodicDispatchRoot(
        m_eventSequence, ticks);
    dxvk::war3::render::RecordWorldObjectsPhase1PairedTiming(
        m_eventSequence,
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::FlushRoot,
        ticks);
    dxvk::war3::render::RecordWorldObjectsPhase1PairedFlushTerminal(
        m_eventSequence, m_terminal);
  }

  void SetTerminal(
      dxvk::war3::render::WorldObjectsPhase1FlushTerminal terminal) noexcept {
    m_terminal = terminal;
  }

  WorldObjectsPhase1PeriodicDispatchRootScope(
      const WorldObjectsPhase1PeriodicDispatchRootScope&) = delete;
  WorldObjectsPhase1PeriodicDispatchRootScope& operator=(
      const WorldObjectsPhase1PeriodicDispatchRootScope&) = delete;

private:
  uint64_t m_eventSequence = 0u;
  int64_t m_begin = 0;
  dxvk::war3::render::WorldObjectsPhase1FlushTerminal m_terminal =
      dxvk::war3::render::WorldObjectsPhase1FlushTerminal::Unclassified;
};

int __fastcall Hook_RenderQueue_Dispatch_Common(void *thisPtr, void *edx,
                                                void *a3, void *a4, void *a5) {
  // Common Dispatch 热路径策略：
  // - 无追踪需求时走快速直通；
  // - 需要追踪时走 ExecBatchProcessor 桥接并恢复状态。
  // Dispatch runs hundreds of times per frame. One-in-eight fixed-period
  // sampling keeps the native/custom split statistically dense while avoiding
  // paired QPC reads on every draw.
  War3HotHookCallTiming aggregatedHookTiming(
      War3HotHookId::DispatchCommon, 8u);
  auto perfScope = MakeRenderHookDrawScope("Hook_Dispatch_Common");
  WorldObjectsPhase1PeriodicDispatchScope phase1DispatchScope(false);
  const uint32_t currentDrawLayerIndex =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a3));
  CurrentDrawDispatchScope currentDrawDispatchScope(
      thisPtr, edx, currentDrawLayerIndex);
  War3BatchTag tag = War3RenderState::GetTlsBatchTag();
  phase1DispatchScope.SetStageTag(tag);
  int elementStage = -1;
  NativeGpuSkinDispatchSemantic gpuSkinSemantic = {};
  const bool b1KnownNegative =
      dxvk::war3::gpu_skin::NativeGpuSkinB1DispatchKnownNegative(
          dxvk::war3::gpu_skin::NativeDispatchPath::Common,
          reinterpret_cast<uintptr_t>(edx), currentDrawLayerIndex);
  if (!b1KnownNegative) {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchResolveSemantic);
    gpuSkinSemantic = ResolveNativeGpuSkinDispatchSemantic(
        edx, currentDrawLayerIndex);
  }
  WorldObjectsPhase1PairedTimingScope nativeBeginTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::DispatchNativeBegin);
  WorldObjectsPhase1NativeEndBracket nativeEndBracket;
  dxvk::war3::gpu_skin::NativeDispatchScope nativeGpuSkinDispatchScope(
      dxvk::war3::gpu_skin::NativeDispatchPath::Common,
      reinterpret_cast<uintptr_t>(thisPtr),
      reinterpret_cast<uintptr_t>(edx), currentDrawLayerIndex,
      gpuSkinSemantic.stage, gpuSkinSemantic.batchTag,
      gpuSkinSemantic.forceFailClosed, b1KnownNegative);
  nativeBeginTiming.Stop();
  WorldObjectsPhase1NativeEndArm nativeEndArm(nativeEndBracket);
  // The scope encloses the original call's 0x6F13A63D batch begin and
  // unconditional 0x6F13A6BE tail flush, so its destructor finalizes the last
  // active upload only after the corresponding 0x6F0EEA43 DIP can occur.
  if constexpr (dxvk::war3::internal::kShadowSemanticDispatchContractProbeEnabled) {
    dxvk::war3::render::VisibleRenderableRecord visible = {};
    dxvk::war3::reimpl::RenderBatchElement syntheticBatch = {};
    syntheticBatch.renderablePart = edx;
    syntheticBatch.layerIndex = currentDrawLayerIndex;
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

  const bool needsObjectTracking = War3RenderState::NeedsObjectTracking();
  const bool needsShadowObjectIdentity =
      War3RenderState::NeedsShadowObjectIdentity();
  const bool needsShadowFallbackBridge =
      War3RenderState::NeedsShadowDrawFallbackBridge();
  const bool needsShadowSemanticTracking =
      War3RenderState::NeedsShadowSemanticTracking();
  const bool needsBatchTracking = War3RenderState::IsBatchTagTrackingEnabled();
  const bool needsProbe = dxvk::war3::render::NativeRendererProbe::IsEnabled();
  bool isWorldBridgeTag = dxvk::war3::hooks::IsWorldBridgeTag(tag);
  const bool worldFastEligibleIgnoringIdentity =
      dxvk::war3::internal::kNativeHookFastPathEnabled &&
      needsShadowSemanticTracking && !needsObjectTracking &&
      !needsShadowFallbackBridge && !needsProbe &&
      isWorldBridgeTag;
  phase1DispatchScope.SetWorldFastEligibility(
      worldFastEligibleIgnoringIdentity, needsShadowObjectIdentity);

  auto publishVisibleAfterDispatch = [&]() {
    const bool worldSemanticTag =
        isWorldBridgeTag ||
        dxvk::War3RenderState::IsWorldObjectPhase();
    if (!needsShadowSemanticTracking || !worldSemanticTag)
      return;
    PublishVisibleRenderableFromDispatch(thisPtr, edx, currentDrawLayerIndex,
                                         nullptr);
  };
  auto callOriginalTimed = [&]() {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchOriginal);
    auto nativeScope =
        MakeRenderHookDrawScope("Hook_Dispatch_Common/NativeOriginal");
    War3HotHookNativeScope nativeTiming(aggregatedHookTiming);
    return g_trampolineDispatchCommon(thisPtr, edx, a3, a4, a5);
  };
  auto publishVisibleTimed = [&]() {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchPublishVisible);
    publishVisibleAfterDispatch();
  };

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeWhenNoTracking &&
      !needsObjectTracking && !needsShadowSemanticTracking &&
      !needsBatchTracking && !needsProbe) {
    const int res = callOriginalTimed();
    publishVisibleTimed();
    War3RenderState::SetTlsDispatchHandle(0);
    War3RenderState::SetTlsBatchHandle(0);
    return res;
  }

  const bool canUseWorldGroupTagFastPath =
      needsShadowSemanticTracking && !needsObjectTracking &&
      !needsShadowObjectIdentity && !needsShadowFallbackBridge && !needsProbe &&
      isWorldBridgeTag;

  if (elementStage < 0 &&
      (!dxvk::war3::internal::kNativeHookFastPathEnabled ||
       needsObjectTracking || needsShadowSemanticTracking ||
       needsBatchTracking || needsProbe) &&
      !canUseWorldGroupTagFastPath) {
    auto &tracker = dxvk::war3::render::RenderQueueTracker::instance();
    tracker.GetTagStage(edx, tag, elementStage);
    isWorldBridgeTag = dxvk::war3::hooks::IsWorldBridgeTag(tag);
  }

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeForNonWorldTag &&
      (needsObjectTracking || needsShadowSemanticTracking) &&
      !needsBatchTracking && !needsProbe &&
      tag != War3BatchTag::Unknown &&
      !isWorldBridgeTag) {
    const int res = callOriginalTimed();
    publishVisibleTimed();
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
    const int res = callOriginalTimed();
    publishVisibleTimed();
    scope.end();
    War3RenderState::SetTlsDispatchHandle(0);
    return res;
  }

  dxvk::war3::render::ExecBatchContext ctx = {};
  {
    auto beginScope = MakeRenderHookDrawScope("Hook_Dispatch_Common/BridgeBegin");
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchExecBegin);
    ctx = dxvk::war3::render::ExecBatchProcessor::Begin(edx, tag, elementStage,
                                                        false);
  }
  int res = 0;
  res = callOriginalTimed();
  {
    auto publishScope =
        MakeRenderHookDrawScope("Hook_Dispatch_Common/PublishVisible");
    publishVisibleTimed();
  }
  {
    auto endScope = MakeRenderHookDrawScope("Hook_Dispatch_Common/BridgeEnd");
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchExecEnd);
    dxvk::war3::render::ExecBatchProcessor::End(ctx);
  }
  War3RenderState::SetTlsDispatchHandle(0);
  return res;
}

int __fastcall Hook_RenderQueue_Dispatch_Special(void *thisPtr, void *edx,
                                                 void *a3, void *a4) {
  // Special Dispatch 与 Common Dispatch 对齐同一套快速路径与桥接策略。
  War3HotHookCallTiming aggregatedHookTiming(
      War3HotHookId::DispatchSpecial, 8u);
  auto perfScope = MakeRenderHookDrawScope("Hook_Dispatch_Special");
  WorldObjectsPhase1PeriodicDispatchScope phase1DispatchScope(true);
  const uint32_t currentDrawLayerIndex =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a3));
  CurrentDrawDispatchScope currentDrawDispatchScope(
      thisPtr, edx, currentDrawLayerIndex);
  if constexpr (dxvk::war3::internal::kShadowSemanticDispatchContractProbeEnabled) {
    dxvk::war3::render::VisibleRenderableRecord visible = {};
    dxvk::war3::reimpl::RenderBatchElement syntheticBatch = {};
    syntheticBatch.renderablePart = edx;
    syntheticBatch.layerIndex = currentDrawLayerIndex;
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
  phase1DispatchScope.SetStageTag(tag);
  int elementStage = -1;
  NativeGpuSkinDispatchSemantic gpuSkinSemantic = {};
  {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchResolveSemantic);
    gpuSkinSemantic = ResolveNativeGpuSkinDispatchSemantic(
        edx, currentDrawLayerIndex);
  }
  WorldObjectsPhase1PairedTimingScope nativeBeginTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::DispatchNativeBegin);
  WorldObjectsPhase1NativeEndBracket nativeEndBracket;
  dxvk::war3::gpu_skin::NativeDispatchScope nativeGpuSkinDispatchScope(
      dxvk::war3::gpu_skin::NativeDispatchPath::Special,
      reinterpret_cast<uintptr_t>(thisPtr),
      reinterpret_cast<uintptr_t>(edx), currentDrawLayerIndex,
      gpuSkinSemantic.stage, gpuSkinSemantic.batchTag,
      gpuSkinSemantic.forceFailClosed);
  nativeBeginTiming.Stop();
  WorldObjectsPhase1NativeEndArm nativeEndArm(nativeEndBracket);
  const bool needsObjectTracking = War3RenderState::NeedsObjectTracking();
  const bool needsShadowObjectIdentity =
      War3RenderState::NeedsShadowObjectIdentity();
  const bool needsShadowFallbackBridge =
      War3RenderState::NeedsShadowDrawFallbackBridge();
  const bool needsShadowSemanticTracking =
      War3RenderState::NeedsShadowSemanticTracking();
  const bool needsBatchTracking = War3RenderState::IsBatchTagTrackingEnabled();
  const bool needsProbe = dxvk::war3::render::NativeRendererProbe::IsEnabled();
  bool isWorldBridgeTag = dxvk::war3::hooks::IsWorldBridgeTag(tag);
  const bool worldFastEligibleIgnoringIdentity =
      dxvk::war3::internal::kNativeHookFastPathEnabled &&
      needsShadowSemanticTracking && !needsObjectTracking &&
      !needsShadowFallbackBridge && !needsProbe &&
      isWorldBridgeTag;
  phase1DispatchScope.SetWorldFastEligibility(
      worldFastEligibleIgnoringIdentity, needsShadowObjectIdentity);

  auto publishVisibleAfterDispatch = [&]() {
    if (!needsShadowSemanticTracking || !isWorldBridgeTag)
      return;
    PublishVisibleRenderableFromDispatch(thisPtr, edx, currentDrawLayerIndex,
                                         nullptr);
  };
  auto callOriginalTimed = [&]() {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchOriginal);
    auto nativeScope =
        MakeRenderHookDrawScope("Hook_Dispatch_Special/NativeOriginal");
    War3HotHookNativeScope nativeTiming(aggregatedHookTiming);
    return g_trampolineDispatchSpecial(thisPtr, edx, a3, a4);
  };
  auto publishVisibleTimed = [&]() {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchPublishVisible);
    publishVisibleAfterDispatch();
  };

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeWhenNoTracking &&
      !needsObjectTracking && !needsShadowSemanticTracking &&
      !needsBatchTracking && !needsProbe) {
    const int res = callOriginalTimed();
    publishVisibleTimed();
    War3RenderState::SetTlsDispatchHandle(0);
    War3RenderState::SetTlsBatchHandle(0);
    return res;
  }

  const bool canUseWorldGroupTagFastPath =
      needsShadowSemanticTracking && !needsObjectTracking &&
      !needsShadowObjectIdentity && !needsShadowFallbackBridge && !needsProbe &&
      isWorldBridgeTag;

  if (elementStage < 0 &&
      (!dxvk::war3::internal::kNativeHookFastPathEnabled ||
       needsObjectTracking || needsShadowSemanticTracking ||
       needsBatchTracking || needsProbe) &&
      !canUseWorldGroupTagFastPath) {
    auto &tracker = dxvk::war3::render::RenderQueueTracker::instance();
    tracker.GetTagStage(edx, tag, elementStage);
    isWorldBridgeTag = dxvk::war3::hooks::IsWorldBridgeTag(tag);
  }

  if (dxvk::war3::internal::kNativeHookFastPathEnabled &&
      dxvk::war3::internal::kNativeHookFastPathSkipBridgeForNonWorldTag &&
      (needsObjectTracking || needsShadowSemanticTracking) &&
      !needsBatchTracking && !needsProbe &&
      tag != War3BatchTag::Unknown &&
      !isWorldBridgeTag) {
    const int res = callOriginalTimed();
    publishVisibleTimed();
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
    const int res = callOriginalTimed();
    publishVisibleTimed();
    scope.end();
    War3RenderState::SetTlsDispatchHandle(0);
    return res;
  }

  dxvk::war3::render::ExecBatchContext ctx = {};
  {
    auto beginScope = MakeRenderHookDrawScope("Hook_Dispatch_Special/BridgeBegin");
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchExecBegin);
    ctx = dxvk::war3::render::ExecBatchProcessor::Begin(edx, tag, elementStage,
                                                        true);
  }
  int res = 0;
  res = callOriginalTimed();
  {
    auto publishScope =
        MakeRenderHookDrawScope("Hook_Dispatch_Special/PublishVisible");
    publishVisibleTimed();
  }
  {
    auto endScope = MakeRenderHookDrawScope("Hook_Dispatch_Special/BridgeEnd");
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchExecEnd);
    dxvk::war3::render::ExecBatchProcessor::End(ctx);
  }
  War3RenderState::SetTlsDispatchHandle(0);
  return res;
}

int __fastcall Reimpl_ObservedDispatchCommon(void* sceneNode,
                                              void* renderablePart,
                                              void* layerIndexValue,
                                              void* a4, void* a5) {
  War3HotHookCallTiming aggregatedHookTiming(
      War3HotHookId::DispatchCommon, 8u);
  auto perfScope = MakeRenderHookDrawScope("Hook_Dispatch_Common");
  WorldObjectsPhase1PeriodicDispatchScope phase1DispatchScope(false);
  phase1DispatchScope.SetStageTag(War3RenderState::GetTlsBatchTag());
  const uint32_t liveLayerIndex =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(layerIndexValue));
  NativeGpuSkinDispatchSemantic gpuSkinSemantic = {};
  const bool b1KnownNegative =
      dxvk::war3::gpu_skin::NativeGpuSkinB1DispatchKnownNegative(
          dxvk::war3::gpu_skin::NativeDispatchPath::Common,
          reinterpret_cast<uintptr_t>(renderablePart), liveLayerIndex);
  if (!b1KnownNegative) {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchResolveSemantic);
    gpuSkinSemantic = ResolveNativeGpuSkinDispatchSemantic(
        renderablePart, liveLayerIndex);
  }
  WorldObjectsPhase1PairedTimingScope nativeBeginTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::DispatchNativeBegin);
  WorldObjectsPhase1NativeEndBracket nativeEndBracket;
  dxvk::war3::gpu_skin::NativeDispatchScope scope(
      dxvk::war3::gpu_skin::NativeDispatchPath::Common,
      reinterpret_cast<uintptr_t>(sceneNode),
      reinterpret_cast<uintptr_t>(renderablePart),
      liveLayerIndex, gpuSkinSemantic.stage, gpuSkinSemantic.batchTag,
      gpuSkinSemantic.forceFailClosed, b1KnownNegative);
  nativeBeginTiming.Stop();
  WorldObjectsPhase1NativeEndArm nativeEndArm(nativeEndBracket);
  WorldObjectsPhase1PairedTimingScope originalTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::DispatchOriginal);
  int result = 0;
  {
    auto nativePerfScope =
        MakeRenderHookDrawScope("Hook_Dispatch_Common/NativeOriginal");
    War3HotHookNativeScope nativeTiming(aggregatedHookTiming);
    result = g_trampolineDispatchCommon != nullptr
        ? g_trampolineDispatchCommon(sceneNode, renderablePart,
                                     layerIndexValue, a4, a5)
        : 0;
  }
  originalTiming.Stop();
  return result;
}

int __fastcall Reimpl_ObservedDispatchSpecial(void* sceneNode,
                                               void* renderablePart,
                                               void* layerIndexValue,
                                               void* a4) {
  War3HotHookCallTiming aggregatedHookTiming(
      War3HotHookId::DispatchSpecial, 8u);
  auto perfScope = MakeRenderHookDrawScope("Hook_Dispatch_Special");
  WorldObjectsPhase1PeriodicDispatchScope phase1DispatchScope(true);
  phase1DispatchScope.SetStageTag(War3RenderState::GetTlsBatchTag());
  const uint32_t liveLayerIndex =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(layerIndexValue));
  NativeGpuSkinDispatchSemantic gpuSkinSemantic = {};
  {
    WorldObjectsPhase1PairedTimingScope timing(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::DispatchResolveSemantic);
    gpuSkinSemantic = ResolveNativeGpuSkinDispatchSemantic(
        renderablePart, liveLayerIndex);
  }
  WorldObjectsPhase1PairedTimingScope nativeBeginTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::DispatchNativeBegin);
  WorldObjectsPhase1NativeEndBracket nativeEndBracket;
  dxvk::war3::gpu_skin::NativeDispatchScope scope(
      dxvk::war3::gpu_skin::NativeDispatchPath::Special,
      reinterpret_cast<uintptr_t>(sceneNode),
      reinterpret_cast<uintptr_t>(renderablePart),
      liveLayerIndex, gpuSkinSemantic.stage, gpuSkinSemantic.batchTag,
      gpuSkinSemantic.forceFailClosed);
  nativeBeginTiming.Stop();
  WorldObjectsPhase1NativeEndArm nativeEndArm(nativeEndBracket);
  WorldObjectsPhase1PairedTimingScope originalTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::DispatchOriginal);
  int result = 0;
  {
    auto nativePerfScope =
        MakeRenderHookDrawScope("Hook_Dispatch_Special/NativeOriginal");
    War3HotHookNativeScope nativeTiming(aggregatedHookTiming);
    result = g_trampolineDispatchSpecial != nullptr
        ? g_trampolineDispatchSpecial(sceneNode, renderablePart,
                                      layerIndexValue, a4)
        : 0;
  }
  originalTiming.Stop();
  return result;
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
  WorldObjectsPhase1PairedTimingScope timing(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::ReimplExecBegin);
  auto *ctx = reinterpret_cast<dxvk::war3::render::ExecBatchContext *>(outCtx);
  *ctx = dxvk::war3::render::ExecBatchProcessor::Begin(element, tag,
                                                       elementStage, isType3);
}

static void Reimpl_ExecEndValue(void *ctx) {
  // 对应 Begin 的收口，确保状态机在 reimpl 路径也能正确恢复。
  if (!ctx)
    return;
  WorldObjectsPhase1PairedTimingScope timing(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::ReimplExecEnd);
  auto *typed = reinterpret_cast<dxvk::war3::render::ExecBatchContext *>(ctx);
  dxvk::war3::render::ExecBatchProcessor::End(*typed);
}

static void NotifyNativeGpuSkinFlush() {
  WorldObjectsPhase1PairedTimingScope pairedTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::FlushNotify);
  if (!dxvk::war3::gpu_skin::NativeBridgeHooksEnabled())
    return;

  dxvk::war3::gpu_skin::NativeFlushObservation observation = {};
  observation.opaqueArray = reinterpret_cast<uintptr_t>(
      g_batchArrayPtr != nullptr ? *g_batchArrayPtr : nullptr);
  observation.transparentArray = reinterpret_cast<uintptr_t>(
      g_transparentArrayBasePtr != nullptr ? *g_transparentArrayBasePtr
                                          : nullptr);
  observation.opaqueCount =
      g_numOfElementsPtr != nullptr ? *g_numOfElementsPtr : 0u;
  observation.transparentCount =
      g_numOfTransparentPtr != nullptr ? *g_numOfTransparentPtr : 0u;
  dxvk::war3::gpu_skin::NotifyNativeFlush(observation);
}

static int CallOriginalFlushSortedItems() {
  auto origScope =
      MakeRenderHookFrameScope("Hook_FlushSortedItems/NativeOriginal");
  War3HotHookBoundaryScope nativeHotHookBoundary;
  WorldObjectsPhase1PairedTimingScope pairedTiming(
      dxvk::war3::render::
          WorldObjectsPhase1PairedTimingStage::FlushOriginalBody);
  // 优先调用 trampoline，保持 Hook 链一致；original 作为兜底。
  int result = 0;
  if (g_trampolineFlushSortedItems)
    result = g_trampolineFlushSortedItems();
  else if (g_originalFlushSortedItems)
    result = g_originalFlushSortedItems();
  return result;
}

int __cdecl Hook_FlushSortedItems() {
  WorldObjectsPhase1PeriodicDispatchRootScope phase1DispatchRootScope;
  const uint64_t phase1CaptureSequence =
      dxvk::war3::render::
          CurrentWorldObjectsPhase1PurePeriodicDispatchSequence();
  if (phase1CaptureSequence != 0u) {
    dxvk::war3::render::RecordWorldObjectsPhase1PairedFlushTopology(
        phase1CaptureSequence,
        g_numOfElementsPtr != nullptr ? *g_numOfElementsPtr : 0u,
        g_numOfTransparentPtr != nullptr ? *g_numOfTransparentPtr : 0u);
  }
  NativeGpuSkinDetourPin detourPin(NativeGpuSkinDetourKind::Flush);
  // FlushSortedItems 支持可选“队列接管模式”：
  // - 关闭接管：调用原函数；
  // - 开启接管：走 reimpl RenderQueue 路径。
  auto perfScope = MakeRenderHookFrameScope("Hook_FlushSortedItems");
  War3HotHookBoundaryScope aggregatedHookFlushScope;
  // Cover publication and both traversal paths with one flush transaction so
  // the manager may publish only into this exact current-flush lifetime.  The
  // first observed render-thread flush cannot acquire the cover until Notify
  // establishes that identity; its local view therefore rejects fail-closed,
  // while later flushes enter here at depth one.
  NativeGpuSkinBridgeFlushScope bridgeFlushScope;
  // Publish once before the native/reimplementation fork so both traversal
  // paths share the same flush epoch and queue snapshot.
  NotifyNativeGpuSkinFlush();
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
    phase1DispatchRootScope.SetTerminal(
        dxvk::war3::render::
            WorldObjectsPhase1FlushTerminal::MissingGlobalsOriginal);
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

  const bool observeGpuSkinDispatch =
      dxvk::war3::gpu_skin::NativeBridgeHooksEnabled();
  auto dispatchCommon =
      observeGpuSkinDispatch
          ? &Reimpl_ObservedDispatchCommon
          : g_trampolineDispatchCommon
          ? g_trampolineDispatchCommon
          : reinterpret_cast<DispatchCommonFn>(g_originalExecBatch0);
  auto dispatchSpecial =
      observeGpuSkinDispatch
          ? &Reimpl_ObservedDispatchSpecial
          : g_trampolineDispatchSpecial
          ? g_trampolineDispatchSpecial
          : reinterpret_cast<DispatchSpecialFn>(g_originalExecBatch3);

  // 若关键 dispatch 缺失则回退原生路径，保证稳定性优先。
  if (!dispatchCommon || !dispatchSpecial) {
    phase1DispatchRootScope.SetTerminal(
        dxvk::war3::render::
            WorldObjectsPhase1FlushTerminal::MissingDispatchOriginal);
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
    phase1DispatchRootScope.SetTerminal(
        dxvk::war3::render::
            WorldObjectsPhase1FlushTerminal::DecisionFallbackOriginal);
    return CallOriginalFlushSortedItems();
  }

  // 执行 opaque + transparent 两段 flush，语义与原生队列流程保持一致。
  bool ok = false;
  {
    WorldObjectsPhase1PairedTimingScope pairedTiming(
        dxvk::war3::render::
            WorldObjectsPhase1PairedTimingStage::FlushReimplOpaque);
    ok = dxvk::war3::reimpl::RenderQueue::FlushSortedItems_StdSort(
        device, globals, g_renderQueueItemComparator, dispatchCommon,
        dispatchSpecial, fns);
  }
  if (!ok) {
    phase1DispatchRootScope.SetTerminal(
        dxvk::war3::render::
            WorldObjectsPhase1FlushTerminal::OpaqueFailureOriginal);
    maybeLogTakeoverGate("opaque-failed", &decision, device, dispatchCommon,
                         dispatchSpecial);
    return CallOriginalFlushSortedItems();
  }

  const uint32_t transparentCount =
      g_numOfTransparentPtr ? *g_numOfTransparentPtr : 0u;
  if (transparentCount > 0u) {
    bool transparentOk = false;
    {
      WorldObjectsPhase1PairedTimingScope pairedTiming(
          dxvk::war3::render::
              WorldObjectsPhase1PairedTimingStage::FlushReimplTransparent);
      if (decision.mode == War3QueueTakeoverMode::Full &&
          dxvk::war3::internal::
              kNativeQueueTakeoverUseNativeTransparentFlush &&
          g_originalFlushTransparent) {
        g_originalFlushTransparent();
        transparentOk = true;
      } else {
        transparentOk =
            dxvk::war3::reimpl::RenderQueue::FlushTransparent_StdSort(
                globals, g_renderQueueItemComparator, fns);
        if (!transparentOk && g_originalFlushTransparent) {
          g_originalFlushTransparent();
          transparentOk = true;
        }
      }
    }

    if (!transparentOk) {
      phase1DispatchRootScope.SetTerminal(
          dxvk::war3::render::
              WorldObjectsPhase1FlushTerminal::TransparentFailureOriginal);
      maybeLogTakeoverGate("transparent-failed", &decision, device,
                           dispatchCommon, dispatchSpecial);
      return CallOriginalFlushSortedItems();
    }
  }

  phase1DispatchRootScope.SetTerminal(
      dxvk::war3::render::
          WorldObjectsPhase1FlushTerminal::TakeoverSuccess);
  return 0;
}

void __fastcall Hook_Terrain_RenderAllTiles(void *thisPtr, void * /*edx*/) {
  // 地形入口仅负责地形层标记，确保后续分类逻辑能识别 terrain phase。
  auto perfScope = MakeRenderHookFrameScope("Hook_Terrain_RenderAllTiles");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_HOOK_HOTPATH_LOG(
        "DXVK War3Hook: Hook_Terrain_RenderAllTiles FIRST_CALL this=0x%p\n",
        thisPtr);
  }

  War3RenderState::OnTerrainEnter();
  {
    auto nativeScope =
        MakeRenderHookFrameScope("Hook_Terrain_RenderAllTiles/NativeOriginal");
    War3HotHookBoundaryScope nativeHotHooks;
    if (g_trampolineTerrainAllTiles) {
      g_trampolineTerrainAllTiles(thisPtr, nullptr);
    } else if (g_originalTerrainAllTiles) {
      g_originalTerrainAllTiles(thisPtr, nullptr);
    }
  }
  War3RenderState::OnTerrainExit();
}

static void RegisterRenderPerfHookCatalog(
    const War3HookAddressBook& book) {
  const War3HookActivationGate deepGate = {
      "kNativePerfDetailHookTimingEnabled",
      "DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS=1", 2u, "",
      "RenderPerf",
      "kNativePerfDetailHookTimingEnabled && PERF_LEVEL>=2 && "
      "DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS=1"};
  const War3HookActivationGate residualGate = {
      "kNativePerfDetailHookTimingEnabled",
      "DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS=1", 2u, "",
      "RenderPerf",
      "kNativePerfDetailHookTimingEnabled && PERF_LEVEL>=2 && "
      "DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS=1"};
  const War3HookActivationGate coreGate = {
      "kNativePerfDetailHookTimingEnabled",
      "DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS=1", 2u, "",
      "RenderPerf",
      "kNativePerfDetailHookTimingEnabled && PERF_LEVEL>=2 && "
      "DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS=1"};
  const War3HookActivationGate renderQueueGate = {
      "kNativePerfDetailHookTimingEnabled",
      "DXVK_WAR3_PERF_RENDERQUEUE_DEEP_HOOKS=1", 0u, "",
      "RenderPerf",
      "kNativePerfDetailHookTimingEnabled && "
      "DXVK_WAR3_PERF_RENDERQUEUE_DEEP_HOOKS=1"};
  const War3HookActivationGate transparentGate = {
      "kNativePerfDetailHookTimingEnabled",
      "DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS=1", 0u, "",
      "RenderPerf",
      "kNativePerfDetailHookTimingEnabled && "
      "DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS=1"};
  const War3HookActivationGate unsafeGate = {
      "", "", 0u, "", "RenderPerf",
      "catalog-only: ordinary C++ MinHook detour cannot preserve caller EDI"};

#define WAR3_RENDERPERF_DESCRIPTOR(                                             \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  {War3HookId::id, "RenderPerf", hookName, War3HookKind::MinHookDetour,         \
   book.bookField, timingRoot, "trampoline", "ObserverOverhead",               \
   "VerifiedABIObserver", deepGate},
#define WAR3_RENDERPERF_RESIDUAL_DESCRIPTOR(                                    \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  {War3HookId::id, "RenderPerf", hookName, War3HookKind::MinHookDetour,         \
   book.bookField, timingRoot, "trampoline", "ObserverOverhead",               \
   "VerifiedABIObserver", residualGate},
#define WAR3_RENDERPERF_CORE_DESCRIPTOR(                                        \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  {War3HookId::id, "RenderPerf", hookName, War3HookKind::MinHookDetour,         \
   book.bookField, timingRoot, "trampoline", "ObserverOverhead",               \
   "VerifiedABIObserver", coreGate},
#define WAR3_RENDERPERF_QUEUE_DESCRIPTOR(                                       \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  {War3HookId::id, "RenderPerf", hookName, War3HookKind::MinHookDetour,         \
   book.bookField, timingRoot, "trampoline", "ObserverOverhead",               \
   "VerifiedABIObserver", renderQueueGate},
#define WAR3_RENDERPERF_TRANSPARENT_DESCRIPTOR(                                 \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  {War3HookId::id, "RenderPerf", hookName, War3HookKind::MinHookDetour,         \
   book.bookField, timingRoot, "trampoline", "ObserverOverhead",               \
   "OptInABIObserver", transparentGate},

  const War3HookDescriptor descriptors[] = {
      WAR3_RENDERPERF_WORLD_PREPARE_DEEP_HOOKS(
          WAR3_RENDERPERF_DESCRIPTOR)
      WAR3_RENDERPERF_WORLD_PREPARE_RESIDUAL_HOOKS(
          WAR3_RENDERPERF_RESIDUAL_DESCRIPTOR)
      {War3HookId::RenderPerfWorldPrepareUnsafeImplicitEdi368E90,
       "RenderPerf", "WorldPrepare_UnsafeImplicitEdi_368E90",
       War3HookKind::MinHookDetour, 0x368E90u,
       "Hook_WorldPrepare_UnsafeImplicitEdi_368E90",
       "unavailable-unsafe-abi", "ObserverOverhead",
       "UnsafeImplicitRegisterABI", unsafeGate},
      WAR3_RENDERPERF_WORLD_PREPARE_CORE_HOOKS(
          WAR3_RENDERPERF_CORE_DESCRIPTOR)
      WAR3_RENDERPERF_RENDERQUEUE_DEEP_HOOKS(
          WAR3_RENDERPERF_QUEUE_DESCRIPTOR)
      WAR3_RENDERPERF_TRANSPARENT_DISPATCH_HOOKS(
          WAR3_RENDERPERF_TRANSPARENT_DESCRIPTOR)
  };
  RegisterHookDescriptors(descriptors,
                          sizeof(descriptors) / sizeof(descriptors[0]));

#undef WAR3_RENDERPERF_TRANSPARENT_DESCRIPTOR
#undef WAR3_RENDERPERF_QUEUE_DESCRIPTOR
#undef WAR3_RENDERPERF_CORE_DESCRIPTOR
#undef WAR3_RENDERPERF_RESIDUAL_DESCRIPTOR
#undef WAR3_RENDERPERF_DESCRIPTOR
}

void War3HookRender::Install(uintptr_t gameBase) {
  const auto &book = GetWar3HookAddressBook127a();
  RegisterRenderPerfHookCatalog(book);
  RecordHookCatalogState(
      War3HookId::RenderPerfWorldPrepareUnsafeImplicitEdi368E90,
      War3HookCatalogStatus::SkippedUnsafeABI,
      reinterpret_cast<LPVOID>(gameBase + 0x368E90u), nullptr, nullptr, 0,
      "callee consumes caller EDI as an implicit argument; catalog-only",
      false);
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
  LPVOID applyDrawStateAndSamplerPairAddr =
      resolveCode(book.applyDrawStateAndSamplerPair);
  LPVOID applyDrawStateAndDrawAddr = resolveCode(book.applyDrawStateAndDraw);
  LPVOID gxDeviceD3dDynamicVertexUploadAddr =
      resolveCode(book.gxDeviceD3dDynamicVertexUpload);
  LPVOID gxDeviceD3dSkinCopyKernelAddr = resolveCode(
      dxvk::war3::gpu_skin::kNativeSkinCopyKernelRva);
  LPVOID flushSortedAddr = resolveCode(book.flushSortedItems);
  LPVOID flushTransparentAddr = resolveCode(book.rqFlushTransparent);
  LPVOID terrainAllTilesAddr = resolveCode(book.terrainRenderAllTiles);
  LPVOID worldPrepareCameraBuildFrustumAddr =
      resolveCode(book.worldPrepareCameraBuildFrustum);
  LPVOID worldPrepareTerrainShadowFlushAddr =
      resolveCode(book.worldPrepareTerrainShadowFlush);
  LPVOID worldPrepareTerrainExtraPassAddr =
      resolveCode(book.worldPrepareTerrainExtraPass);
  LPVOID worldPrepareShadowProjectorFlushAddr =
      resolveCode(book.worldPrepareShadowProjectorFlush);
  LPVOID worldPrepareTargetIndicatorRingAdvanceAddr =
      resolveCode(book.worldPrepareTargetIndicatorRingAdvance);
  LPVOID worldPrepareCinematicFilterTimeAdvanceAddr =
      resolveCode(book.worldPrepareCinematicFilterTimeAdvance);
  LPVOID worldPrepareRuntimeFlagClockAdvance3B8760Addr =
      resolveCode(book.worldPrepareRuntimeFlagClockAdvance3B8760);
  LPVOID worldPrepareFlushDeferredSelectionObjectsAddr =
      resolveCode(book.worldPrepareFlushDeferredSelectionObjects);
  LPVOID worldPrepareGlobalRenderCallbackPassAddr =
      resolveCode(book.worldPrepareGlobalRenderCallbackPass);
  LPVOID worldPrepareRenderWaypointIndicatorsAddr =
      resolveCode(book.worldPrepareRenderWaypointIndicators);
  LPVOID worldPrepareFrameUpdateGateAddr =
      resolveCode(book.worldPrepareFrameUpdateGate);
  LPVOID worldPrepareGameUiFrameSyncAddr =
      resolveCode(book.worldPrepareGameUiFrameSync);
  LPVOID worldPrepareUpdateIndicatorAnchorAddr =
      resolveCode(book.worldPrepareUpdateIndicatorAnchor);
  LPVOID worldPrepareCameraAdvanceAddr =
      resolveCode(book.worldPrepareCameraAdvance);
  LPVOID worldPrepareCameraPrepareConstantsAddr =
      resolveCode(book.worldPrepareCameraPrepareConstants);
  LPVOID worldPrepareViewProjPrepareAddr =
      resolveCode(book.worldPrepareViewProjPrepare);
  LPVOID worldPrepareSceneQueryFlushSyncAddr =
      resolveCode(book.worldPrepareSceneQueryFlushSync);
  LPVOID worldPrepareFixedPointRemapAddr =
      resolveCode(book.worldPrepareFixedPointRemap);
  LPVOID worldPreparePostVisibilityGlobalAdvanceAAddr =
      resolveCode(book.worldPreparePostVisibilityGlobalAdvanceA);
  LPVOID worldPreparePostVisibilityFrameAnchorUpdateAddr =
      resolveCode(book.worldPreparePostVisibilityFrameAnchorUpdate);
  LPVOID worldPreparePostVisibilityFrameAnchorVisibilityQueryAddr =
      resolveCode(book.worldPreparePostVisibilityFrameAnchorVisibilityQuery);
  LPVOID worldPreparePostVisibilityGlobalAdvanceBAddr =
      resolveCode(book.worldPreparePostVisibilityGlobalAdvanceB);
  LPVOID worldPrepareVisibilityTailAdvanceAAddr =
      resolveCode(book.worldPrepareVisibilityTailAdvanceA);
  LPVOID worldPrepareVisibilityTailAdvanceBAddr =
      resolveCode(book.worldPrepareVisibilityTailAdvanceB);
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

  dxvk::war3::gpu_skin::NativeBridgeAddresses nativeBridgeAddresses = {};
  nativeBridgeAddresses.flushSortedItemsRva = book.flushSortedItems;
  nativeBridgeAddresses.dispatchCommonRva = book.dispatchCommon;
  nativeBridgeAddresses.applyDrawStateAndSamplerPairRva =
      book.applyDrawStateAndSamplerPair;
  nativeBridgeAddresses.gxDeviceD3dDynamicVertexUploadRva =
      book.gxDeviceD3dDynamicVertexUpload;
  nativeBridgeAddresses.gxDeviceD3dSkinCopyKernelRva =
      dxvk::war3::gpu_skin::kNativeSkinCopyKernelRva;
  // This must run before any Render-domain detour changes the opcode bytes
  // included in the exact Game.dll fingerprint.
  const bool nativeBridgeInstallEligible =
      dxvk::war3::gpu_skin::InitializeNativeBridge(
          gameBase, nativeBridgeAddresses);
  const auto nativeBridgeConfig =
      dxvk::war3::gpu_skin::GetNativeBridgeRuntimeConfig();
  if (nativeBridgeConfig.mode !=
      dxvk::war3::gpu_skin::GpuSkinMode::Disabled) {
    const auto fingerprint =
        dxvk::war3::gpu_skin::GetNativeBridgeFingerprint();
    char md5[33] = {};
    char sha1[41] = {};
    auto formatDigest = [](const uint8_t* bytes, size_t byteCount,
                           char* output) {
      constexpr char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < byteCount; ++i) {
        output[i * 2u] = hex[bytes[i] >> 4u];
        output[i * 2u + 1u] = hex[bytes[i] & 0x0Fu];
      }
      output[byteCount * 2u] = '\0';
    };
    formatDigest(fingerprint.md5, sizeof(fingerprint.md5), md5);
    formatDigest(fingerprint.sha1, sizeof(fingerprint.sha1), sha1);
    const char* imageBaseKind = "invalid";
    if ((fingerprint.peValidMask &
         dxvk::war3::gpu_skin::NativePeFingerprintImageBasePreferred) != 0u) {
      imageBaseKind = "preferred";
    } else if ((fingerprint.peValidMask &
                dxvk::war3::gpu_skin::
                    NativePeFingerprintImageBaseRuntimeRelocated) != 0u) {
      imageBaseKind = "runtime-relocated";
    }

    char gpuSkinInitLine[256] = {};
    std::snprintf(
        gpuSkinInitLine, sizeof(gpuSkinInitLine),
        "DXVK War3Hook[GpuSkin]: mode=%u fingerprintExact=%d "
        "failureMask=0x%08x peValidMask=0x%08x peFailureMask=0x%08x "
        "opcodeFailureMask=0x%08x installEligible=%d",
        static_cast<unsigned>(nativeBridgeConfig.mode),
        fingerprint.exactMatch ? 1 : 0, fingerprint.failureMask,
        fingerprint.peValidMask, fingerprint.peFailureMask,
        fingerprint.opcodeFailureMask,
        nativeBridgeInstallEligible ? 1 : 0);
    war3dbg::Print(
        "DXVK War3Hook[GpuSkin]: mode=%u fingerprintExact=%d "
        "failureMask=0x%08x peValidMask=0x%08x peFailureMask=0x%08x "
        "opcodeFailureMask=0x%08x\n",
        static_cast<unsigned>(nativeBridgeConfig.mode),
        fingerprint.exactMatch ? 1 : 0, fingerprint.failureMask,
        fingerprint.peValidMask, fingerprint.peFailureMask,
        fingerprint.opcodeFailureMask);
    ::dxvk::Logger::info(gpuSkinInitLine);

    char gpuSkinPeLine[384] = {};
    std::snprintf(
        gpuSkinPeLine, sizeof(gpuSkinPeLine),
        "DXVK War3Hook[GpuSkin]: fingerprint PE loadedBase=%p "
        "headerImageBase=0x%08x imageBaseKind=%s e_lfanew=0x%08x "
        "ntSignature=0x%08x machine=0x%04x magic=0x%04x timestamp=0x%08x "
        "sizeOfImage=0x%08x checksum=0x%08x",
        reinterpret_cast<void*>(fingerprint.loadedImageBase),
        fingerprint.observedImageBase, imageBaseKind,
        fingerprint.observedPeOffset,
        fingerprint.observedNtSignature,
        static_cast<unsigned>(fingerprint.observedMachine),
        static_cast<unsigned>(fingerprint.observedOptionalMagic),
        fingerprint.peTimestamp, fingerprint.imageSize,
        fingerprint.imageChecksum);
    ::dxvk::Logger::info(gpuSkinPeLine);

    char gpuSkinFileLine[256] = {};
    std::snprintf(
        gpuSkinFileLine, sizeof(gpuSkinFileLine),
        "DXVK War3Hook[GpuSkin]: fingerprint file version=%u.%u.%u.%u "
        "size=%llu md5=%s sha1=%s",
        fingerprint.fileVersion[0], fingerprint.fileVersion[1],
        fingerprint.fileVersion[2], fingerprint.fileVersion[3],
        static_cast<unsigned long long>(fingerprint.fileSize), md5, sha1);
    ::dxvk::Logger::info(gpuSkinFileLine);
  }

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
  g_originalApplyDrawStateAndDraw =
      reinterpret_cast<ApplyDrawStateAndDrawFn>(applyDrawStateAndDrawAddr);
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
#define WAR3_INSTALL_RENDERPERF_CATALOG_HOOK(                                   \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  InstallMinHook(                                                              \
      War3HookId::id, resolveCode(book.bookField),                              \
      reinterpret_cast<LPVOID>(&detour),                                        \
      reinterpret_cast<LPVOID*>(&trampoline), "RenderPerf", hookName, false,    \
      false);
#define WAR3_MARK_RENDERPERF_CATALOG_HOOK(                                      \
    id, hookName, bookField, detour, trampoline, timingRoot)                    \
  RecordHookCatalogState(                                                      \
      War3HookId::id, disabledCatalogState, resolveCode(book.bookField),        \
      reinterpret_cast<LPVOID>(&detour),                                        \
      reinterpret_cast<LPVOID>(trampoline), 0, disabledCatalogReason, false);
  if (War3WorldPrepareDeepTimingHooksRuntimeEnabled()) {
    InstallMinHook(
        worldPrepareCameraBuildFrustumAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_CameraBuildFrustum),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareCameraBuildFrustum),
        "RenderPerf", "WorldPrepare_CameraBuildFrustum", false, false);
    InstallMinHook(
        worldPrepareTerrainShadowFlushAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_TerrainShadowFlush),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareTerrainShadowFlush),
        "RenderPerf", "WorldPrepare_TerrainShadowFlush", false, false);
    InstallMinHook(
        worldPrepareTerrainExtraPassAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_TerrainExtraPass),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareTerrainExtraPass),
        "RenderPerf", "WorldPrepare_TerrainExtraPass", false, false);
    InstallMinHook(
        worldPrepareShadowProjectorFlushAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_ShadowProjectorFlush),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareShadowProjectorFlush),
        "RenderPerf", "WorldPrepare_ShadowProjectorFlush", false, false);
    InstallMinHook(
        worldPrepareTargetIndicatorRingAdvanceAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_TargetIndicatorRingAdvance),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareTargetIndicatorRingAdvance),
        "RenderPerf", "WorldPrepare_TargetIndicatorRingAdvance", false, false);
    InstallMinHook(
        worldPrepareCinematicFilterTimeAdvanceAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_CinematicFilterTimeAdvance),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareCinematicFilterTimeAdvance),
        "RenderPerf", "WorldPrepare_CinematicFilterTimeAdvance", false, false);
    InstallMinHook(
        worldPrepareRuntimeFlagClockAdvance3B8760Addr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareRuntimeFlagClockAdvance3B8760),
        "RenderPerf", "WorldPrepare_RuntimeFlagClockAdvance3B8760", false,
        false);
  } else {
    const auto disabledCatalogState =
        dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? War3HookCatalogStatus::DisabledByEnvironment
            : War3HookCatalogStatus::DisabledByCompileConfig;
    const char* disabledCatalogReason =
        !dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? "kNativePerfDetailHookTimingEnabled=false"
            : (dxvk::war3::internal::War3PerfHookLevel() < 2
                   ? "PERF_LEVEL below required level 2"
                   : "DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS disabled");
    WAR3_RENDERPERF_WORLD_PREPARE_DEEP_HOOKS(
        WAR3_MARK_RENDERPERF_CATALOG_HOOK)
  }
  if (War3WorldPrepareResidualTimingHooksRuntimeEnabled()) {
    InstallMinHook(
        worldPrepareFlushDeferredSelectionObjectsAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_FlushDeferredSelectionObjects),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareFlushDeferredSelectionObjects),
        "RenderPerf", "WorldPrepare_FlushDeferredSelectionObjects", false,
        false);
    InstallMinHook(
        worldPrepareGlobalRenderCallbackPassAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_GlobalRenderCallbackPass),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareGlobalRenderCallbackPass),
        "RenderPerf", "WorldPrepare_GlobalRenderCallbackPass", false, false);
    InstallMinHook(
        worldPrepareRenderWaypointIndicatorsAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_RenderWaypointIndicators),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareRenderWaypointIndicators),
        "RenderPerf", "WorldPrepare_RenderWaypointIndicators", false, false);
  } else {
    const auto disabledCatalogState =
        dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? War3HookCatalogStatus::DisabledByEnvironment
            : War3HookCatalogStatus::DisabledByCompileConfig;
    const char* disabledCatalogReason =
        !dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? "kNativePerfDetailHookTimingEnabled=false"
            : (dxvk::war3::internal::War3PerfHookLevel() < 2
                   ? "PERF_LEVEL below required level 2"
                   : "DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS disabled");
    WAR3_RENDERPERF_WORLD_PREPARE_RESIDUAL_HOOKS(
        WAR3_MARK_RENDERPERF_CATALOG_HOOK)
  }
  if (War3WorldPrepareCoreTimingHooksRuntimeEnabled()) {
    InstallMinHook(
        worldPrepareFrameUpdateGateAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_FrameUpdateGate),
        reinterpret_cast<LPVOID*>(&g_trampolineWorldPrepareFrameUpdateGate),
        "RenderPerf", "WorldPrepare_FrameUpdateGate", false, false);
    InstallMinHook(
        worldPrepareGameUiFrameSyncAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_GameUiFrameSync),
        reinterpret_cast<LPVOID*>(&g_trampolineWorldPrepareGameUiFrameSync),
        "RenderPerf", "WorldPrepare_GameUiFrameSync", false, false);
    InstallMinHook(
        worldPrepareUpdateIndicatorAnchorAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_UpdateIndicatorAnchor),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareUpdateIndicatorAnchor),
        "RenderPerf", "WorldPrepare_UpdateIndicatorAnchor", false, false);
    InstallMinHook(
        worldPrepareCameraAdvanceAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_CameraAdvance),
        reinterpret_cast<LPVOID*>(&g_trampolineWorldPrepareCameraAdvance),
        "RenderPerf", "WorldPrepare_CameraAdvance", false, false);
    InstallMinHook(
        worldPrepareCameraPrepareConstantsAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_CameraPrepareConstants),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareCameraPrepareConstants),
        "RenderPerf", "WorldPrepare_CameraPrepareConstants", false, false);
    InstallMinHook(
        worldPrepareViewProjPrepareAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_ViewProjPrepare),
        reinterpret_cast<LPVOID*>(&g_trampolineWorldPrepareViewProjPrepare),
        "RenderPerf", "WorldPrepare_ViewProjPrepare", false, false);
    InstallMinHook(
        worldPrepareSceneQueryFlushSyncAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_SceneQueryFlushSync),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareSceneQueryFlushSync),
        "RenderPerf", "WorldPrepare_SceneQueryFlushSync", false, false);
    InstallMinHook(
        worldPrepareFixedPointRemapAddr,
        reinterpret_cast<LPVOID>(&Hook_WorldPrepare_FixedPointRemap),
        reinterpret_cast<LPVOID*>(&g_trampolineWorldPrepareFixedPointRemap),
        "RenderPerf", "WorldPrepare_FixedPointRemap", false, false);
    InstallMinHook(
        worldPreparePostVisibilityGlobalAdvanceAAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_PostVisibilityGlobalAdvanceA),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPreparePostVisibilityGlobalAdvanceA),
        "RenderPerf", "WorldPrepare_PostVisibilityGlobalAdvanceA", false,
        false);
    InstallMinHook(
        worldPreparePostVisibilityFrameAnchorUpdateAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_PostVisibility_FrameAnchorUpdate),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPreparePostVisibilityFrameAnchorUpdate),
        "RenderPerf", "WorldPrepare_PostVisibility_FrameAnchorUpdate", false,
        false);
    InstallMinHook(
        worldPreparePostVisibilityFrameAnchorVisibilityQueryAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPreparePostVisibilityFrameAnchorVisibilityQuery),
        "RenderPerf",
        "WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery", false,
        false);
    InstallMinHook(
        worldPreparePostVisibilityGlobalAdvanceBAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_PostVisibilityGlobalAdvanceB),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPreparePostVisibilityGlobalAdvanceB),
        "RenderPerf", "WorldPrepare_PostVisibilityGlobalAdvanceB", false,
        false);
    InstallMinHook(
        worldPrepareVisibilityTailAdvanceAAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_VisibilityTailAdvanceA),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareVisibilityTailAdvanceA),
        "RenderPerf", "WorldPrepare_VisibilityTailAdvanceA", false, false);
    InstallMinHook(
        worldPrepareVisibilityTailAdvanceBAddr,
        reinterpret_cast<LPVOID>(
            &Hook_WorldPrepare_VisibilityTailAdvanceB),
        reinterpret_cast<LPVOID*>(
            &g_trampolineWorldPrepareVisibilityTailAdvanceB),
        "RenderPerf", "WorldPrepare_VisibilityTailAdvanceB", false, false);
  } else {
    const auto disabledCatalogState =
        dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? War3HookCatalogStatus::DisabledByEnvironment
            : War3HookCatalogStatus::DisabledByCompileConfig;
    const char* disabledCatalogReason =
        !dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? "kNativePerfDetailHookTimingEnabled=false"
            : (dxvk::war3::internal::War3PerfHookLevel() < 2
                   ? "PERF_LEVEL below required level 2"
                   : "DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS disabled");
    WAR3_RENDERPERF_WORLD_PREPARE_CORE_HOOKS(
        WAR3_MARK_RENDERPERF_CATALOG_HOOK)
  }
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
  const bool dispatchCommonHookInstalled = InstallMinHook(
      dispatchCommonAddr,
      reinterpret_cast<LPVOID>(&Hook_RenderQueue_Dispatch_Common),
      reinterpret_cast<LPVOID *>(&g_trampolineDispatchCommon), "Render",
      "RenderQueue_Dispatch_Common", false, false);
  InstallMinHook(dispatchSpecialAddr,
                 reinterpret_cast<LPVOID>(&Hook_RenderQueue_Dispatch_Special),
                 reinterpret_cast<LPVOID *>(&g_trampolineDispatchSpecial),
                 "Render", "RenderQueue_Dispatch_Special", false, false);
  // This detour was historically expensive even with an empty probe. Preserve
  // the original business-probe gate instead of charging every draw in normal
  // or frame-level profiling.
  if (War3PreparedSliceProbeRuntimeEnabled()) {
    InstallMinHook(applyDrawStateAndDrawAddr,
                   reinterpret_cast<LPVOID>(&Hook_ApplyDrawStateAndDraw),
                   reinterpret_cast<LPVOID *>(
                       &g_trampolineApplyDrawStateAndDraw),
                   "RenderPerf", "ApplyDrawStateAndDraw", false, false);
  }
  const bool flushSortedHookInstalled = InstallMinHook(
      flushSortedAddr, reinterpret_cast<LPVOID>(&Hook_FlushSortedItems),
      reinterpret_cast<LPVOID *>(&g_trampolineFlushSortedItems), "Render",
      "FlushSortedItems", false, false);
  if (War3RenderQueueDeepTimingHooksRuntimeEnabled()) {
    WAR3_RENDERPERF_RENDERQUEUE_DEEP_HOOKS(
        WAR3_INSTALL_RENDERPERF_CATALOG_HOOK)
  } else {
    const auto disabledCatalogState =
        dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? War3HookCatalogStatus::DisabledByEnvironment
            : War3HookCatalogStatus::DisabledByCompileConfig;
    const char* disabledCatalogReason =
        !dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? "kNativePerfDetailHookTimingEnabled=false"
            : "DXVK_WAR3_PERF_RENDERQUEUE_DEEP_HOOKS disabled";
    WAR3_RENDERPERF_RENDERQUEUE_DEEP_HOOKS(
        WAR3_MARK_RENDERPERF_CATALOG_HOOK)
  }
  if (War3TransparentDispatchTimingHooksRuntimeEnabled()) {
    WAR3_RENDERPERF_TRANSPARENT_DISPATCH_HOOKS(
        WAR3_INSTALL_RENDERPERF_CATALOG_HOOK)
  } else {
    const auto disabledCatalogState =
        dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? War3HookCatalogStatus::DisabledByEnvironment
            : War3HookCatalogStatus::DisabledByCompileConfig;
    const char* disabledCatalogReason =
        !dxvk::war3::internal::kNativePerfDetailHookTimingEnabled
            ? "kNativePerfDetailHookTimingEnabled=false"
            : "DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS disabled";
    WAR3_RENDERPERF_TRANSPARENT_DISPATCH_HOOKS(
        WAR3_MARK_RENDERPERF_CATALOG_HOOK)
  }
#undef WAR3_MARK_RENDERPERF_CATALOG_HOOK
#undef WAR3_INSTALL_RENDERPERF_CATALOG_HOOK
  InstallMinHook(terrainAllTilesAddr,
                 reinterpret_cast<LPVOID>(&Hook_Terrain_RenderAllTiles),
                 reinterpret_cast<LPVOID *>(&g_trampolineTerrainAllTiles),
                 "Render", "Terrain_RenderAllTiles", false, false);

  if (nativeBridgeInstallEligible && dispatchCommonHookInstalled &&
      flushSortedHookInstalled) {
    g_gpuSkinDetourActivity.state.store(
        NativeGpuSkinDetourState::Installing, std::memory_order_release);
    g_gpuSkinDetourActivity.admitFlushPins.store(
        false, std::memory_order_release);
    // MH_CreateHook leaves each target disabled. Create the complete owned set
    // before one queued transaction is allowed to expose any detour.
    const bool applyHookCreated = CreateNativeGpuSkinHook(
        applyDrawStateAndSamplerPairAddr,
        reinterpret_cast<LPVOID>(&Hook_ApplyDrawStateAndSamplerPair),
        reinterpret_cast<LPVOID *>(
            &g_trampolineApplyDrawStateAndSamplerPair),
        "ApplyDrawStateAndSamplerPair_138EE0", g_gpuSkinApplyHook);
    const bool uploadHookCreated = CreateNativeGpuSkinHook(
        gxDeviceD3dDynamicVertexUploadAddr,
        reinterpret_cast<LPVOID>(&Hook_GxDeviceD3dDynamicVertexUpload),
        reinterpret_cast<LPVOID *>(
            &g_trampolineGxDeviceD3dDynamicVertexUpload),
        "GxDeviceD3dDynamicVertexUpload_0EEA50", g_gpuSkinUploadHook);
    const bool kernelHookCreated = CreateNativeGpuSkinHook(
        gxDeviceD3dSkinCopyKernelAddr,
        reinterpret_cast<LPVOID>(&Hook_GxDeviceD3dSkinCopyKernel),
        reinterpret_cast<LPVOID *>(
            &g_trampolineGxDeviceD3dSkinCopyKernel),
        "GxDeviceD3dSkinCopyKernel_0EDDC0", g_gpuSkinKernelHook);
    const bool hookSetCreated =
        applyHookCreated && uploadHookCreated && kernelHookCreated;
    const bool hooksEnabled = hookSetCreated &&
        QueueEnableNativeGpuSkinHooks();

    if (hooksEnabled) {
      // Render hooks and Game.dll are process-lifetime in this runtime. Map
      // transitions reset managers, but must not remove these detours.
      g_gpuSkinDetourActivity.state.store(
          NativeGpuSkinDetourState::Running, std::memory_order_release);
      g_gpuSkinDetourActivity.admitFlushPins.store(
          true, std::memory_order_release);
      dxvk::war3::gpu_skin::SetNativeBridgeHooksInstalled(true);
      char gpuSkinHookLine[256] = {};
      std::snprintf(
          gpuSkinHookLine, sizeof(gpuSkinHookLine),
          "DXVK War3Hook[GpuSkin]: native three-hook transaction installed "
          "apply=%p upload=%p kernel=%p",
          applyDrawStateAndSamplerPairAddr,
          gxDeviceD3dDynamicVertexUploadAddr,
          gxDeviceD3dSkinCopyKernelAddr);
      ::dxvk::Logger::info(gpuSkinHookLine);
      if (auto* activeDevice = dxvk::war3::GetActiveDevice())
        activeDevice->War3AttachGpuSkinNativeBridge(gameBase);
    } else {
      const bool rollbackComplete =
          UninstallNativeGpuSkinHookTransaction();
      war3dbg::Print(
          "DXVK War3Hook[GpuSkin]: native hook transaction failed "
          "(apply=%d upload=%d kernel=%d enable=%d rollback=%d); "
          "group disabled\n",
          applyHookCreated ? 1 : 0, uploadHookCreated ? 1 : 0,
          kernelHookCreated ? 1 : 0, hooksEnabled ? 1 : 0,
          rollbackComplete ? 1 : 0);
      char gpuSkinHookLine[224] = {};
      std::snprintf(
          gpuSkinHookLine, sizeof(gpuSkinHookLine),
          "DXVK War3Hook[GpuSkin]: native three-hook transaction failed "
          "apply=%d upload=%d kernel=%d enable=%d rollback=%d; "
          "group disabled",
          applyHookCreated ? 1 : 0, uploadHookCreated ? 1 : 0,
          kernelHookCreated ? 1 : 0, hooksEnabled ? 1 : 0,
          rollbackComplete ? 1 : 0);
      ::dxvk::Logger::info(gpuSkinHookLine);
    }
  } else {
    dxvk::war3::gpu_skin::SetNativeBridgeHooksInstalled(false);
    if (nativeBridgeInstallEligible) {
      war3dbg::Print(
          "DXVK War3Hook[GpuSkin]: observer disabled because required "
          "dispatch/flush hook installation failed (dispatch=%d flush=%d)\n",
          dispatchCommonHookInstalled ? 1 : 0,
          flushSortedHookInstalled ? 1 : 0);
    }
    if (nativeBridgeConfig.mode !=
        dxvk::war3::gpu_skin::GpuSkinMode::Disabled) {
      char gpuSkinHookLine[224] = {};
      std::snprintf(
          gpuSkinHookLine, sizeof(gpuSkinHookLine),
          "DXVK War3Hook[GpuSkin]: native three-hook transaction disabled "
          "eligible=%d dispatch=%d flush=%d; group not installed",
          nativeBridgeInstallEligible ? 1 : 0,
          dispatchCommonHookInstalled ? 1 : 0,
          flushSortedHookInstalled ? 1 : 0);
      ::dxvk::Logger::info(gpuSkinHookLine);
    }
  }

  War3HookRenderIdentity::Install(gameBase);
}

#undef WAR3_RENDERPERF_TRANSPARENT_DISPATCH_HOOKS
#undef WAR3_RENDERPERF_RENDERQUEUE_DEEP_HOOKS
#undef WAR3_RENDERPERF_WORLD_PREPARE_CORE_HOOKS
#undef WAR3_RENDERPERF_WORLD_PREPARE_RESIDUAL_HOOKS
#undef WAR3_RENDERPERF_WORLD_PREPARE_DEEP_HOOKS

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
