#include "war3_runtime_bootstrap.h"

#include "../../d3d9_war3_debug.h"

#include "../core/war3_internal_test_config.h"
#include "../core/war3_net_event_hook.h"
#include "../memory/war3_shadow_arena.h"
#include "../memory/war3_storm_hook.h"
#include "../memory/war3_tlsf_pool.h"
#include "../model/war3_model_hook.h"
#include "../render/war3_render_exec_batch.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_renderer.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../shader/war3_shader_manager.h"
#include "../state/war3_render_state.h"
#include "../war3.h"

namespace dxvk::war3::platform {

void InitializeRuntimeCore(uintptr_t gameBase) {
  if (gameBase != 0) {
    dxvk::war3::Initialize(gameBase);
    dxvk::war3::ShaderManager::get().reload();
  } else {
    dxvk::war3::Initialize();
  }

  dxvk::war3::NetEventHook::get().init();

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

  if (dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
      !dxvk::war3::memory::ShadowArena_IsInitialized())
    dxvk::war3::memory::ShadowArena_Init();
}

void ResetRuntimeCore() {
  dxvk::war3::render::RenderQueueTracker::instance().Reset();
  dxvk::war3::render::ExecBatchProcessor::ResetCaches();

  dxvk::War3RenderState::ResetRuntimeState();

  dxvk::war3::model::Shutdown();
  dxvk::war3::render::ResetShadowRuntimeBridgeState();
  dxvk::war3::render::War3Renderer::instance().EndFrame();
  dxvk::war3::render::War3Renderer::instance().BeginFrame();
  dxvk::war3::memory::ShadowArena_Reset();
  dxvk::war3::state::RenderState::instance().setWorldPointer(nullptr);
  dxvk::war3::state::RenderState::instance().setIsInGame(false);
  dxvk::war3::state::RenderState::instance().setIsLoading(false);

  dxvk::war3::NetEventHook::get().cleanup();
}

} // namespace dxvk::war3::platform
