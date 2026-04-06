#include "war3_hook_ui.h"
#include "war3_hook_address_book.h"
#include "war3_hook_install_util.h"

#include "../../d3d9_war3_debug.h"
#include "../../jass/war3_game.h"

#include "../core/war3_internal_test_config.h"
#include "../render/war3_render_dispatcher.h"
#include "../render/war3_render_state.h"
#include "../tools/war3_perf_monitor.h"

#include <d3d9.h>
#include <windows.h>

namespace dxvk::war3::hooks {

namespace {

// ---------------------------------------------------------------------------
// UI 域局部常量与函数类型定义
// ---------------------------------------------------------------------------

// 帧率上限覆盖（基于 MemHack 的游戏设置接口）
static constexpr uint32_t kWar3MaxFpsOverride =
    dxvk::war3::internal::kWar3UiMaxFpsOverrideValue;
static constexpr bool kWar3OverrideMaxFps =
    dxvk::war3::internal::kWar3UiOverrideMaxFpsEnabled;

using GetD3d9ParametersFn = DWORD(__fastcall *)(void *, void *,
                                                D3DPRESENT_PARAMETERS *);
using UiDispatchFn = int(__fastcall *)(void *, void *, int, std::int64_t, int);
using UiRenderableRenderFn = int(__fastcall *)(void *, void *, int, int);

GetD3d9ParametersFn g_originalGetD3d9Parameters = nullptr;
UiDispatchFn g_originalUiDispatch = nullptr;
UiDispatchFn g_trampolineUiDispatch = nullptr;
UiRenderableRenderFn g_originalUiRenderableRender = nullptr;
UiRenderableRenderFn g_trampolineUiRenderableRender = nullptr;

/**
 * @brief UI 热路径可选性能分段。
 *
 * 默认关闭细粒度采样时返回空 Scope，避免在 UI 高频调用中产生
 * PerfMonitor 路径维护与原子开销。
 */
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeUiHotpathCpuScope(const char *name) {
  if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
    return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

/**
 * @brief 修改指定地址的 1 字节补丁。
 *
 * 用于 FPS 解锁补丁写入；写入后会刷新指令缓存并恢复页属性。
 */
bool PatchByte(uintptr_t addr, uint8_t value) {
  DWORD oldProtect = 0;
  if (!VirtualProtect(reinterpret_cast<void *>(addr), 1, PAGE_EXECUTE_READWRITE,
                      &oldProtect))
    return false;

  *reinterpret_cast<uint8_t *>(addr) = value;
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(addr), 1);

  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void *>(addr), 1, oldProtect, &ignored);
  return true;
}

DWORD __fastcall Hook_GetD3d9Parameters(void *thisPtr, void *edx,
                                        D3DPRESENT_PARAMETERS *params) {
  // 先执行原逻辑，再覆盖 presentation interval，确保不破坏原参数填充流程。
  DWORD result = 0;
  if (g_originalGetD3d9Parameters)
    result = g_originalGetD3d9Parameters(thisPtr, edx, params);
  if (params && dxvk::war3::internal::kWar3ForceImmediatePresentEnabled)
    params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  return result;
}

#define WAR3_HOOK_HOTPATH_LOG(fmt, ...)                                        \
  do {                                                                         \
    if (war3dbg::RenderLogEnabled()) {                                         \
      WAR3_RENDER_LOG(fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

int __fastcall Hook_UiDispatch(void *self, void *edx, int a2, std::int64_t a3,
                               int a4) {
  // 步骤1：在 UI 分发入口尝试执行 FPS 覆盖逻辑（带内部重试/幂等保护）。
  War3TryOverrideMaxFps();

  // 步骤2：当未启用 batch 追踪时走快速路径，避免进入额外桥接逻辑。
  if constexpr (dxvk::war3::internal::kNativeHookFastPathEnabled) {
    if (!War3RenderState::IsBatchTagTrackingEnabled()) {
      if (g_trampolineUiDispatch)
        return g_trampolineUiDispatch(self, edx, a2, a3, a4);
      if (g_originalUiDispatch)
        return g_originalUiDispatch(self, edx, a2, a3, a4);
      return 0;
    }
  }

  auto perfScope = MakeUiHotpathCpuScope("Hook_UiDispatch");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_HOOK_HOTPATH_LOG("DXVK War3Hook: Hook_UiDispatch FIRST_CALL self=0x%p "
                          "a2=%d a3=0x%llX a4=%d\n",
                          self, a2, static_cast<unsigned long long>(a3), a4);
  }

  // 步骤3：进入 UI 分发状态机，写入 dispatcher 层级上下文。
  const auto prevLayer =
      dxvk::war3::render::War3RenderDispatcher::instance().BeginUiDispatch();

  // 步骤4：执行原函数并保留返回值。
  int result = 0;
  if (g_trampolineUiDispatch) {
    auto origScope = MakeUiHotpathCpuScope("Hook_UiDispatch/Orig");
    result = g_trampolineUiDispatch(self, edx, a2, a3, a4);
  } else if (g_originalUiDispatch) {
    auto origScope = MakeUiHotpathCpuScope("Hook_UiDispatch/Orig");
    result = g_originalUiDispatch(self, edx, a2, a3, a4);
  }

  // 步骤5：退出 UI 分发状态机，恢复进入前层级。
  dxvk::war3::render::War3RenderDispatcher::instance().EndUiDispatch(prevLayer);
  return result;
}

int __fastcall Hook_UiRenderableRender(void *node, void *renderCtx, int a3,
                                       int a4) {
  // 快速路径：无需跟踪时直接转发，减少 UI 热路径分支成本。
  if constexpr (dxvk::war3::internal::kNativeHookFastPathEnabled) {
    if (!War3RenderState::IsBatchTagTrackingEnabled()) {
      if (g_trampolineUiRenderableRender)
        return g_trampolineUiRenderableRender(node, renderCtx, a3, a4);
      if (g_originalUiRenderableRender)
        return g_originalUiRenderableRender(node, renderCtx, a3, a4);
      return 0;
    }

    // UI 稳态快退：
    // 当当前层已是 UI 且本帧已观察到 UiDispatch 时，
    // 无需重复执行层切换与 dispatch 补写逻辑，直接透传原函数。
    if (War3RenderState::CurrentLayer() == War3RenderLayer::UI &&
        War3RenderState::HasUiDispatchThisFrame()) {
      if (g_trampolineUiRenderableRender)
        return g_trampolineUiRenderableRender(node, renderCtx, a3, a4);
      if (g_originalUiRenderableRender)
        return g_originalUiRenderableRender(node, renderCtx, a3, a4);
      return 0;
    }
  }

  auto perfScope = MakeUiHotpathCpuScope("Hook_UiRenderableRender");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    auto isReadable = [](const void *p, size_t size) -> bool {
      if (!p || size == 0)
        return false;
      MEMORY_BASIC_INFORMATION mbi = {};
      if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
      if (mbi.State != MEM_COMMIT)
        return false;
      const DWORD prot = mbi.Protect & 0xFFu;
      if (prot == PAGE_NOACCESS)
        return false;
      const bool readable =
          prot == PAGE_READONLY || prot == PAGE_READWRITE ||
          prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
          prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
      if (!readable)
        return false;
      const uintptr_t start = reinterpret_cast<uintptr_t>(p);
      const uintptr_t end = start + size;
      const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
      const uintptr_t regionEnd = regionStart + mbi.RegionSize;
      return end <= regionEnd;
    };

    uintptr_t vtbl = 0;
    uintptr_t renderFn = 0;
    if (isReadable(node, sizeof(uintptr_t))) {
      vtbl = *reinterpret_cast<uintptr_t *>(node);
      if (isReadable(reinterpret_cast<const void *>(vtbl + 0x0C), sizeof(uintptr_t)))
        renderFn = *reinterpret_cast<uintptr_t *>(vtbl + 0x0C);
    }
    WAR3_HOOK_HOTPATH_LOG("DXVK War3Hook: Hook_UiRenderableRender FIRST_CALL "
                          "node=0x%p renderFn=0x%p\n",
                          node, reinterpret_cast<void *>(renderFn));
  }

  // 关键流程：
  // 1) 如果当前帧尚未看到 UiDispatch，则补写一次 UI 标记；
  // 2) 进入 UI 层；
  // 3) 执行原函数；
  // 4) 恢复上层。
  const auto prevLayer = War3RenderState::CurrentLayer();
  const bool needLayerSwitch = prevLayer != War3RenderLayer::UI;
  const bool needUiDispatchMark = !War3RenderState::HasUiDispatchThisFrame();
  if (needUiDispatchMark)
    War3RenderState::OnUiDispatch();
  if (needLayerSwitch)
    War3RenderState::PushUiLayer();

  int result = 0;
  if (g_trampolineUiRenderableRender) {
    auto origScope =
        MakeUiHotpathCpuScope("Hook_UiRenderableRender/Orig");
    result = g_trampolineUiRenderableRender(node, renderCtx, a3, a4);
  } else if (g_originalUiRenderableRender) {
    auto origScope =
        MakeUiHotpathCpuScope("Hook_UiRenderableRender/Orig");
    result = g_originalUiRenderableRender(node, renderCtx, a3, a4);
  }

  if (needLayerSwitch)
    War3RenderState::PopLayer(prevLayer);
  return result;
}

} // namespace

/**
 * @brief 尝试覆盖游戏 FPS 与刷新率。
 *
 * 说明：
 * - 使用静态状态保证幂等；
 * - 限制最大重试次数，避免长期占用热路径；
 * - 对 1.27a 额外应用内存补丁与参数 Hook。
 */
void War3TryOverrideMaxFps() {
  static bool s_done = false;
  static bool s_fpsDone = false;
  static bool s_refreshDone = false;
  static bool s_unlockPatchDone = false;
  static bool s_paramsHookDone = false;
  static uint32_t s_tries = 0;

  if (!kWar3OverrideMaxFps || s_done || s_tries >= 120)
    return;

  s_tries += 1;

  if (!::pGameDLL)
    ::base_game_init();

  // 步骤1：1.27a 特定路径 - 先做 patch，再装 GetD3d9Parameters Hook。
  if (::game_version == 0x27a && ::pGameDLL) {
    const auto &book = GetWar3HookAddressBook127a();
    if (!s_unlockPatchDone) {
      const uintptr_t patchAddr = ::pGameDLL + 0x5FCFB;
      if (PatchByte(patchAddr, 0xFF)) {
        war3dbg::Print("DXVK War3Hook: FPS unlock patch applied @0x%p\n",
                       reinterpret_cast<void *>(patchAddr));
        s_unlockPatchDone = true;
      } else if (s_tries == 1) {
        war3dbg::Print("DXVK War3Hook: FPS unlock patch failed\n");
      }
    }

    if (!s_paramsHookDone &&
        dxvk::war3::internal::kWar3UiInstallD3d9ParamsHookEnabled) {
      const uintptr_t hookAddr = ::pGameDLL + book.getD3d9Parameters;
      if (InstallMinHook(
              reinterpret_cast<LPVOID>(hookAddr),
              reinterpret_cast<LPVOID>(&Hook_GetD3d9Parameters),
              reinterpret_cast<LPVOID *>(&g_originalGetD3d9Parameters), "UI",
              "GetD3d9Parameters", true, false)) {
        s_paramsHookDone = true;
      }
    } else if (!dxvk::war3::internal::kWar3UiInstallD3d9ParamsHookEnabled) {
      // 生命周期域已安装同名 Hook；UI 域默认不重复安装，避免冲突。
      s_paramsHookDone = true;
    }
  }

  // 步骤2：写入 MAX_FPS 配置项。
  if (!s_fpsDone) {
    uint32_t oldValue = 0;
    if (::war3_get_game_opt_value(GAME_OPTION_MAX_FPS, &oldValue)) {
      if (::war3_set_game_opt_value(GAME_OPTION_MAX_FPS, kWar3MaxFpsOverride)) {
        war3dbg::Print("DXVK War3Hook: Max FPS override %u -> %u\n",
                       static_cast<unsigned>(oldValue),
                       static_cast<unsigned>(kWar3MaxFpsOverride));
        s_fpsDone = true;
      } else if (s_tries == 1) {
        war3dbg::Print("DXVK War3Hook: Max FPS override failed\n");
      }
    } else if (s_tries == 1) {
      war3dbg::Print("DXVK War3Hook: Max FPS read failed\n");
    }
  }

  // 步骤3：写入刷新率配置项（读取系统显示模式后覆盖）。
  if (!s_refreshDone &&
      dxvk::war3::internal::kWar3UiOverrideRefreshRateEnabled) {
    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);
    if (!EnumDisplaySettings(nullptr, ENUM_CURRENT_SETTINGS, &dm))
      EnumDisplaySettings(nullptr, ENUM_REGISTRY_SETTINGS, &dm);

    const uint32_t refresh = dm.dmDisplayFrequency;
    if (refresh > 0) {
      if (::war3_set_game_opt_value(GAME_OPTION_REFRESH_RATE, refresh)) {
        war3dbg::Print("DXVK War3Hook: Refresh rate override -> %u Hz\n",
                       static_cast<unsigned>(refresh));
        s_refreshDone = true;
      } else if (s_tries == 1) {
        war3dbg::Print("DXVK War3Hook: Refresh rate override failed\n");
      }
    } else {
      if (s_tries == 1)
        war3dbg::Print("DXVK War3Hook: Refresh rate unavailable\n");
      s_refreshDone = true;
    }
  } else if (!dxvk::war3::internal::kWar3UiOverrideRefreshRateEnabled) {
    // 不强制写刷新率配置，保留用户在游戏内的分辨率/刷新率手动设置。
    s_refreshDone = true;
  }

  const bool extraDone =
      (::game_version != 0x27a) || (s_unlockPatchDone && s_paramsHookDone);
  if (s_fpsDone && s_refreshDone && extraDone)
    s_done = true;
}

/**
 * @brief 安装 UI 域 Hook。
 * @param gameBase Game.dll 基址。
 */
void InstallUiHooks(uintptr_t gameBase) {
  if (!gameBase)
    return;

  const auto &book = GetWar3HookAddressBook127a();
  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  LPVOID uiDispatchAddr = resolveCode(book.uiDispatch);
  LPVOID uiRenderableAddr = resolveCode(book.uiRenderableRender);

  // 先记录原函数指针，再安装 Hook，便于失败时仍可安全回退。
  g_originalUiDispatch = reinterpret_cast<UiDispatchFn>(uiDispatchAddr);
  g_originalUiRenderableRender = reinterpret_cast<UiRenderableRenderFn>(uiRenderableAddr);

  InstallMinHook(uiDispatchAddr, reinterpret_cast<LPVOID>(&Hook_UiDispatch),
                 reinterpret_cast<LPVOID *>(&g_trampolineUiDispatch), "UI",
                 "UiDispatch", true, false);
  InstallMinHook(uiRenderableAddr,
                 reinterpret_cast<LPVOID>(&Hook_UiRenderableRender),
                 reinterpret_cast<LPVOID *>(&g_trampolineUiRenderableRender),
                 "UI", "UiRenderableRender", true, false);
}

} // namespace dxvk::war3::hooks
