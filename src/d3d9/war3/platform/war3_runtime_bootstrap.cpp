#include "war3_runtime_bootstrap.h"

#include "../../d3d9_war3_debug.h"

#include "../core/war3_internal_test_config.h"
#include "../core/war3_net_event_hook.h"
#include "../core/war3_runtime_profile.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../memory/war3_shadow_arena.h"
#include "../memory/war3_storm_hook.h"
#include "../memory/war3_tlsf_pool.h"
#include "../math/war3_curve_runtime.h"
#include "../model/war3_model_hook.h"
#include "../render/war3_render_exec_batch.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_lightning_runtime.h"
#include "../render/war3_renderer.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../shadow/war3_shadow_native_runtime.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../shader/war3_shader_manager.h"
#include "../state/war3_render_state.h"
#include "../tools/war3_control_plane.h"
#include "../war3.h"

namespace dxvk::war3::platform {

namespace {

void AdvanceSmallSemanticBuilds(
    dxvk::war3::shadow::ShadowValidationRuntime& validationRuntime,
    uint32_t maxExtraBuildPasses) {
  validationRuntime.requestLatestFrameBuild();
  validationRuntime.ensureLatestFrameBuilt();

  // Stage-aware native validation must catch the small supplemented unit frame
  // in the same render tick. Keep this bounded so a large diagnostic build does
  // not turn into a frame-time spike.
  constexpr uint64_t kSmallBuildRecordCeiling = 2048u;
  for (uint32_t i = 0u; i < maxExtraBuildPasses; ++i) {
    const auto buildState = validationRuntime.buildStateSnapshot();
    if (!buildState.buildInProgress && !buildState.buildRequestPending)
      break;
    if (buildState.buildInProgress && buildState.buildRecordCount != 0u &&
        buildState.buildRecordCount > kSmallBuildRecordCeiling)
      break;
    validationRuntime.ensureLatestFrameBuilt();
  }
}

} // namespace

void InitializeRuntimeCore(uintptr_t gameBase) {
  dxvk::war3dbg::Print(
      "DXVK War3Bootstrap: 运行时核心初始化开始 gameBase=%p\n",
      reinterpret_cast<void*>(gameBase));
  if (gameBase != 0) {
    dxvk::war3dbg::Print("DXVK War3Bootstrap: war3::Initialize 开始\n");
    dxvk::war3::Initialize(gameBase);
    dxvk::war3dbg::Print("DXVK War3Bootstrap: war3::Initialize 完成\n");
    dxvk::war3dbg::Print("DXVK War3Bootstrap: ShaderManager reload 开始\n");
    dxvk::war3::ShaderManager::get().reload();
    dxvk::war3dbg::Print("DXVK War3Bootstrap: ShaderManager reload 完成\n");
  } else {
    dxvk::war3dbg::Print("DXVK War3Bootstrap: war3::Initialize(auto) 开始\n");
    dxvk::war3::Initialize();
    dxvk::war3dbg::Print("DXVK War3Bootstrap: war3::Initialize(auto) 完成\n");
  }

  dxvk::war3dbg::Print("DXVK War3Bootstrap: NetEventHook 初始化开始\n");
  const bool netEventReady = dxvk::war3::NetEventHook::get().init();
  dxvk::war3dbg::Print(
      "DXVK War3Bootstrap: NetEventHook 初始化完成 ready=%d\n",
      netEventReady ? 1 : 0);
  dxvk::war3dbg::Print("DXVK War3Bootstrap: 控制平面初始化开始\n");
  dxvk::war3::tools::InitializeWar3ControlPlane();
  dxvk::war3dbg::Print(
      "DXVK War3Bootstrap: 控制平面初始化返回 running=%d\n",
      dxvk::war3::tools::IsWar3ControlPlaneRunning() ? 1 : 0);

  if constexpr (dxvk::war3::internal::kWar3StormBreakerEnabled) {
    if (!dxvk::war3::memory::TlsfPool_IsInitialized()) {
      if (dxvk::war3::memory::TlsfPool_Init()) {
        dxvk::war3dbg::Print("DXVK War3[StormHook]: 运行时早期初始化 TLSF\n");
      } else {
        dxvk::war3dbg::Print(
            "DXVK War3[StormHook]: 运行时早期初始化 TLSF 失败\n");
      }
    }

    if (dxvk::war3::memory::TlsfPool_IsInitialized() &&
        !dxvk::war3::memory::StormHook_IsInstalled()) {
      if (dxvk::war3::memory::StormHook_Install()) {
        dxvk::war3dbg::Print("DXVK War3[StormHook]: 运行时早期安装完成\n");
      } else {
        dxvk::war3dbg::Print("DXVK War3[StormHook]: 运行时早期安装失败\n");
      }
    }

    if (dxvk::war3::memory::StormHook_IsInstalled() &&
        !dxvk::war3::memory::StormHook_IsRedirectEnabled()) {
      dxvk::war3::memory::StormHook_SetRedirectEnabled(true);
    }
  } else {
    dxvk::war3dbg::Print("DXVK War3[StormHook]: 二分诊断态跳过运行时初始化\n");
  }

  // ShadowArena is VkDevice-owned. Its initialization is intentionally
  // deferred to the D3D9 render owner, which supplies the exact DxvkDevice;
  // this game-thread bootstrap must never bind process-global pages.
}

void ResetRuntimeCore() {
  // This callback can run on the game/unload thread while the render thread is
  // still recording the old map.  Keep it strictly to map/JASS-owned CPU
  // state.  RenderQueue, model, Lightning, shadow bridge, Receiver, GPU Skin
  // and Arena state are reset together by the Present safe-point transaction.
  dxvk::war3::math::CurveRuntime::instance().reset();
  dxvk::war3::tools::ResetWar3ControlPlaneState();
  dxvk::war3::state::RenderState::instance().setWorldPointer(nullptr);
  dxvk::war3::state::RenderState::instance().setIsInGame(false);
  dxvk::war3::state::RenderState::instance().setIsLoading(false);

  dxvk::war3::NetEventHook::get().cleanup();
}

void BindNativeShadowDevice(IDirect3DDevice9* device) {
  dxvk::war3::shadow::NativeD3D9BackendRuntime::instance().setDevice(device);
  dxvk::war3::render::War3LightningRuntime::instance().setDevice(device);
}

bool DriveNativeShadowBackend(bool captureLiveState,
                              uint32_t maxExtraBuildPasses) {
  if (!dxvk::war3::internal::
          IsNativeRendererHostExecuteValidationRuntimeEnabled())
    return false;
  if (!dxvk::war3::runtime::IsWar3RuntimeModuleEnabled(
          dxvk::war3::runtime::War3RuntimeModule::SemanticData))
    return false;
  if (!dxvk::war3::internal::kWar3RuntimeConfigSemanticConsumerEffective)
    return false;

  if constexpr (dxvk::war3::internal::
                    kNativeSemanticShadowWorldStageValidationEnabled) {
    if (!dxvk::war3::internal::
            IsNativeSemanticShadowWorldStageValidationRuntimeEnabled())
      return false;

    auto& validationRuntime =
        dxvk::war3::shadow::ShadowValidationRuntime::instance();
    if (captureLiveState) {
      dxvk::war3::render::War3Renderer::instance()
          .PublishSemanticRegistriesForScene();
      dxvk::war3::shadow::ShadowRuntimeContractCache::instance()
          .captureLiveState();
    }
    AdvanceSmallSemanticBuilds(validationRuntime, maxExtraBuildPasses);
  }

  return dxvk::war3::shadow::NativeD3D9BackendRuntime::instance()
      .buildLatestFrame();
}

bool ExecuteNativeShadowBackendPreparedFrame() {
  // Lightning owns a separate Stage 20 hook. Executing it from this shadow
  // helper made semantic-shadow configurations draw the same bolt twice and
  // could also bypass the normal world-dispatch a5==0 gate.
  return dxvk::war3::internal::
             IsNativeRendererHostExecuteValidationRuntimeEnabled()
      ? dxvk::war3::shadow::NativeD3D9BackendRuntime::instance()
            .executePreparedFrame()
      : false;
}

} // namespace dxvk::war3::platform
