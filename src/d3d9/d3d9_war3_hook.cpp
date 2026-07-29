#include "d3d9_war3_hook.h"

#include "d3d9_device.h"
#include "d3d9_war3_branding.h"
#include "d3d9_war3_debug.h"
#include "jass/war3_game.h"

#include <MinHook.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <winver.h>

#include "war3/core/war3_events.h"
#include "war3/core/war3_internal_test_config.h"
#include "war3/core/war3_memory.h"
#include "war3/core/war3_net_event_hook.h"
#include "war3/hooks/war3_hook_jass.h"
#include "war3/hooks/war3_hook_address_book.h"
#include "war3/hooks/war3_hook_lifecycle.h"
#include "war3/hooks/war3_hook_render.h"
#include "war3/hooks/war3_hook_shadow.h"
#include "war3/hooks/war3_hook_widget_identity.h"
#include "war3/hooks/war3_hook_ui.h"
#include "war3/memory/war3_storm_hook.h"
#include "war3/memory/war3_tlsf_pool.h"
#include "war3/model/war3_model_hook.h"
#include "war3/native/war3_native_hooks.h"
#include "war3/platform/war3_module_api.h"
#include "war3/platform/war3_runtime_bootstrap.h"
#include "war3/reimpl/war3_render_queue.h"
#include "war3/render/war3_render_exec_batch.h"
#include "war3/render/war3_render_queue_tracker.h"
#include "war3/render/war3_renderer.h"
#include "war3/shader/war3_shader_manager.h"
#include "war3/state/war3_render_state.h"
#include "war3/tools/war3_diagnostics_hub.h"
#include "war3/war3.h"

namespace dxvk {

std::atomic<int> War3Hook::s_currentStage = -1;
bool War3Hook::s_hooksInstalled = false;
thread_local bool War3Hook::s_inShadowPass = false;
IDirect3DDevice9 *War3Hook::s_device = nullptr;

// ---------------------------------------------------------------------------
// 分域 Hook 共享全局（由 d3d9_war3_hook.cpp 统一持有）。
// ---------------------------------------------------------------------------
std::atomic<bool> g_renderQueueGlobalsValid{false};
uint32_t *g_numOfElementsPtr = nullptr;
uint32_t *g_numOfTransparentPtr = nullptr;
void **g_batchArrayPtr = nullptr;
uint32_t *g_sortedBatchCountPtr = nullptr;
void **g_sortedBatchPtrs = nullptr;
uint32_t *g_stateOptEnabledPtr = nullptr;
uint32_t *g_stateCleanupPendingPtr = nullptr;

dxvk::war3::reimpl::ItemComparatorFn g_renderQueueItemComparator = nullptr;
dxvk::war3::reimpl::ApplyStateBlockFn g_renderQueueApplyStateBlock = nullptr;
dxvk::war3::reimpl::StageUpdateFn g_renderQueueStageUpdate = nullptr;
dxvk::war3::reimpl::GxCleanupFn g_renderQueueGxCleanup74 = nullptr;
dxvk::war3::reimpl::GxCleanupFn g_renderQueueGxCleanup78 = nullptr;

// 生命周期域共享状态（由 War3HookLifecycle 使用）。
thread_local bool s_inMainRunner = false;
std::atomic<bool> g_war3_runtime_activated{false};

namespace {

std::atomic<bool> s_nativeTakeoverInstallAttempted{false};

using dxvk::war3::IsExecutableRange;
using dxvk::war3::IsReadableRange;

struct ModuleInfo {
  uintptr_t base = 0;
  uint32_t size = 0;
  uint32_t timestamp = 0;
};

struct FileVersion {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t patch = 0;
  uint16_t build = 0;
};

std::once_flag g_minhookInitOnce;
std::atomic<MH_STATUS> g_minhookInitStatus{MH_UNKNOWN};
std::atomic<bool> g_bootstrapInstalled{false};
constexpr size_t kHookProbeSize = 32;

// ---------------------------------------------------------------------------
// 模块信息与版本探测
// ---------------------------------------------------------------------------

bool GetModuleInfo(HMODULE module, ModuleInfo &out) {
  if (!module)
    return false;

  const uintptr_t base = reinterpret_cast<uintptr_t>(module);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (!IsReadableRange(dos, sizeof(IMAGE_DOS_HEADER)))
    return false;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;

  const uintptr_t ntAddr = base + static_cast<uintptr_t>(dos->e_lfanew);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(ntAddr);
  if (!IsReadableRange(nt, sizeof(IMAGE_NT_HEADERS32)))
    return false;
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return false;
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    return false;

  const uint32_t size = nt->OptionalHeader.SizeOfImage;
  if (size == 0)
    return false;

  out.base = base;
  out.size = size;
  out.timestamp = nt->FileHeader.TimeDateStamp;
  return true;
}

bool QueryModuleFileVersion(HMODULE module, FileVersion &out) {
  if (!module)
    return false;

  char path[MAX_PATH] = {0};
  const DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return false;

  HMODULE versionMod = LoadLibraryA("version.dll");
  if (!versionMod)
    return false;

  using GetFileVersionInfoSizeA_t = DWORD(WINAPI *)(LPCSTR, LPDWORD);
  using GetFileVersionInfoA_t = BOOL(WINAPI *)(LPCSTR, DWORD, DWORD, LPVOID);
  using VerQueryValueA_t = BOOL(WINAPI *)(LPCVOID, LPCSTR, LPVOID *, PUINT);

  auto pGetFileVersionInfoSizeA = reinterpret_cast<GetFileVersionInfoSizeA_t>(
      GetProcAddress(versionMod, "GetFileVersionInfoSizeA"));
  auto pGetFileVersionInfoA = reinterpret_cast<GetFileVersionInfoA_t>(
      GetProcAddress(versionMod, "GetFileVersionInfoA"));
  auto pVerQueryValueA = reinterpret_cast<VerQueryValueA_t>(
      GetProcAddress(versionMod, "VerQueryValueA"));

  if (!pGetFileVersionInfoSizeA || !pGetFileVersionInfoA || !pVerQueryValueA) {
    FreeLibrary(versionMod);
    return false;
  }

  DWORD handle = 0;
  const DWORD size = pGetFileVersionInfoSizeA(path, &handle);
  if (size == 0) {
    FreeLibrary(versionMod);
    return false;
  }

  std::vector<std::uint8_t> data(size);
  if (!pGetFileVersionInfoA(path, handle, size, data.data())) {
    FreeLibrary(versionMod);
    return false;
  }

  VS_FIXEDFILEINFO *fixedInfo = nullptr;
  UINT fixedLen = 0;
  if (!pVerQueryValueA(data.data(), "\\", reinterpret_cast<void **>(&fixedInfo),
                       &fixedLen) ||
      !fixedInfo || fixedLen < sizeof(VS_FIXEDFILEINFO) ||
      fixedInfo->dwSignature != 0xFEEF04BDu) {
    FreeLibrary(versionMod);
    return false;
  }

  out.major = HIWORD(fixedInfo->dwFileVersionMS);
  out.minor = LOWORD(fixedInfo->dwFileVersionMS);
  out.patch = HIWORD(fixedInfo->dwFileVersionLS);
  out.build = LOWORD(fixedInfo->dwFileVersionLS);

  FreeLibrary(versionMod);
  return true;
}

bool IsAddressInModule(uintptr_t addr, const ModuleInfo &mod) {
  const uint64_t end =
      static_cast<uint64_t>(mod.base) + static_cast<uint64_t>(mod.size);
  return mod.base != 0 && mod.size != 0 &&
         static_cast<uint64_t>(addr) >= static_cast<uint64_t>(mod.base) &&
         static_cast<uint64_t>(addr) < end;
}

bool GetEnvBool(const char *name, bool defaultValue) {
  char buf[32] = {0};
  const DWORD len =
      GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
  if (len == 0 || len >= sizeof(buf))
    return defaultValue;

  const char c0 = buf[0];
  if (c0 == '0' || c0 == 'n' || c0 == 'N' || c0 == 'f' || c0 == 'F')
    return false;
  if (c0 == '1' || c0 == 'y' || c0 == 'Y' || c0 == 't' || c0 == 'T')
    return true;

  return defaultValue;
}

void TryInstallStormBreakerEarly(const char *source) {
  if constexpr (!dxvk::war3::internal::kWar3StormBreakerEnabled) {
    static std::atomic<bool> s_logged{false};
    bool expected = false;
    if (s_logged.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
      war3dbg::Print("DXVK War3[StormHook]: 二分诊断态关闭 early install "
                     "source=%s\n",
                     source);
    }
    return;
  }

  if (!dxvk::war3::memory::TlsfPool_IsInitialized()) {
    if (dxvk::war3::memory::TlsfPool_Init()) {
      war3dbg::Print(
          "DXVK War3[StormHook]: 早期初始化 TLSF 完成 source=%s\n", source);
    } else {
      war3dbg::Print(
          "DXVK War3[StormHook]: 早期初始化 TLSF 失败 source=%s\n", source);
      return;
    }
  }

  if (!dxvk::war3::memory::StormHook_IsInstalled()) {
    if (dxvk::war3::memory::StormHook_Install()) {
      war3dbg::Print("DXVK War3[StormHook]: 早期安装完成 source=%s\n", source);
    } else {
      war3dbg::Print("DXVK War3[StormHook]: 早期安装失败 source=%s\n", source);
      return;
    }
  }

  if (!dxvk::war3::memory::StormHook_IsRedirectEnabled())
    dxvk::war3::memory::StormHook_SetRedirectEnabled(true);
}

bool EnsureMinHookInitialized() {
  std::call_once(g_minhookInitOnce, []() {
    const MH_STATUS st = MH_Initialize();
    g_minhookInitStatus.store(st, std::memory_order_relaxed);
  });

  const MH_STATUS st = g_minhookInitStatus.load(std::memory_order_relaxed);
  return st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
}

uintptr_t GetGameDllBase() {
  return reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
}

LPVOID ResolveHookCodeAddress(const ModuleInfo &gameInfo, uintptr_t rva,
                              const char *name, const char *source) {
  const uintptr_t addr = gameInfo.base + rva;
  if (!IsAddressInModule(addr, gameInfo) ||
      !IsExecutableRange(reinterpret_cast<const void *>(addr), kHookProbeSize) ||
      !IsReadableRange(reinterpret_cast<const void *>(addr), kHookProbeSize)) {
    war3dbg::Print("DXVK War3Hook: 跳过 %s (rva=0x%08X addr=%p source=%s) - "
                   "地址不可读/不可执行或版本不匹配\n",
                   name, static_cast<unsigned>(rva),
                   reinterpret_cast<void *>(addr),
                   (source && source[0]) ? source : "(unknown)");
    return nullptr;
  }
  return reinterpret_cast<LPVOID>(addr);
}

dxvk::war3::hooks::ShadowHookAddresses
BuildShadowHookAddresses(const ModuleInfo &gameInfo, const char *source) {
  const auto &book = dxvk::war3::hooks::GetWar3HookAddressBook127a();
  auto resolve = [&](uintptr_t rva, const char *name) -> LPVOID {
    return ResolveHookCodeAddress(gameInfo, rva, name, source);
  };

  dxvk::war3::hooks::ShadowHookAddresses shadowHooks = {};
  shadowHooks.terrainShadowLayerAddr =
      resolve(book.terrainShadowLayer, "Terrain_RenderShadowLayer");
  shadowHooks.terrainRenderListAAddr =
      resolve(book.terrainShadowListA, "TerrainShadow_RenderListA");
  shadowHooks.terrainRenderListBAddr =
      resolve(book.terrainShadowListB, "TerrainShadow_RenderListB");
  shadowHooks.shadowUpdateWriteEntryAddr =
      resolve(book.shadowUpdateWriteEntry, "ShadowUpdate_WriteEntry");
  shadowHooks.shadowProjectorAddSimpleAddr =
      resolve(book.shadowProjectorAddSimple, "ShadowProjector_Add_Simple");
  shadowHooks.shadowProjectorAddFromObjectAddr =
      resolve(book.shadowProjectorAddFromObject, "ShadowProjector_Add_FromObject");
  shadowHooks.shadowRegisterImageEntryAddr =
      resolve(book.shadowRegisterImageEntry, "TerrainShadow_RegisterImageEntry");
  shadowHooks.shadowProjectorSimpleBridgeAddr =
      resolve(book.shadowProjectorSimpleBridge,
              "ShadowPath_ObjectProjector_SimpleBridge");
  shadowHooks.shadowPathObjectProjectorRuntimeAddr =
      resolve(book.shadowPathObjectProjectorRuntime,
              "ShadowPath_ObjectProjector_Runtime");
  shadowHooks.shadowPathObjectProjectorJassBridgeAddr =
      resolve(book.shadowPathObjectProjectorJassBridge,
              "ShadowPath_ObjectProjector_JassBridge");
  shadowHooks.shadowPathStaticStampToggleAddr =
      resolve(book.shadowPathStaticStampToggle, "ShadowPath_StaticStamp_Toggle");
  shadowHooks.cunitUiRecordSetUnitShadowAddr =
      resolve(book.cunitUiRecordSetUnitShadow,
              "CUnitUIManager_RecordSetUnitShadow");
  shadowHooks.cunitUiRecordSetStructureShadowAddr =
      resolve(book.cunitUiRecordSetStructureShadow,
              "CUnitUIManager_RecordSetStructureShadow");
  shadowHooks.shadowRegisterRetWithParamsAddr =
      resolve(book.shadowRegisterRetWithParams, "RegisterImageRet_WithParams");
  shadowHooks.shadowRegisterRetSelectionCircleAddr =
      resolve(book.shadowRegisterRetSelectionCircle,
              "RegisterImageRet_SelectionCircle");
  shadowHooks.shadowRegisterRetStaticStampAddr =
      resolve(book.shadowRegisterRetStaticStamp, "RegisterImageRet_StaticStamp");
  shadowHooks.shadowRegisterRetEmitterStampAddr =
      resolve(book.shadowRegisterRetEmitterStamp, "RegisterImageRet_EmitterStamp");
  shadowHooks.shadowRegisterRetObjectBridgeAddr =
      resolve(book.shadowRegisterRetObjectBridge, "RegisterImageRet_ObjectBridge");
  shadowHooks.shadowRegisterRetMarkOcclusionAddr =
      resolve(book.shadowRegisterRetMarkOcclusion, "RegisterImageRet_MarkOcclusion");
  shadowHooks.shadowRegisterRetFromPointAddr =
      resolve(book.shadowRegisterRetFromPoint, "RegisterImageRet_FromPoint");
  shadowHooks.shadowRegisterRetFromTwoPointsAddr =
      resolve(book.shadowRegisterRetFromTwoPoints, "RegisterImageRet_FromTwoPoints");
  shadowHooks.terrainShadowWriteMaskRegionAddr =
      resolve(book.terrainShadowWriteMaskRegion, "TerrainShadow_WriteMaskRegion");
  shadowHooks.terrainShadowDispatchToShapeAddr =
      resolve(book.terrainShadowDispatchToShape, "TerrainShadow_DispatchToShape");
  shadowHooks.terrainShadowToggleStaticStampFromObjectAddr =
      resolve(book.terrainShadowToggleStaticStampFromObject,
              "TerrainShadow_ToggleStaticStampFromObject");
  shadowHooks.terrainShadowToggleEmitterStampAddr =
      resolve(book.terrainShadowToggleEmitterStamp,
              "TerrainShadow_ToggleEmitterStamp");
  shadowHooks.terrainShadowListARenderPreparedGroupsAddr =
      resolve(book.terrainShadowListARenderPreparedGroups,
              "TerrainShadow_ListA_RenderPreparedGroups");
  shadowHooks.terrainShadowListARenderAllEntriesAddr =
      resolve(book.terrainShadowListARenderAllEntries,
              "Terrain_ShadowListA_RenderAllEntries");
  return shadowHooks;
}

// 地图退出时重置运行时状态，避免跨局残留。
void ResetWar3RuntimeState() {
  if (auto* device = dxvk::war3::GetActiveDevice())
    device->War3ResetGpuSkinMapEpoch();
  g_war3_runtime_activated.store(false, std::memory_order_release);
  std::this_thread::yield();
  dxvk::war3::platform::ResetRuntimeCore();
}

bool ValidateGameModule(uintptr_t gameBase, ModuleInfo &gameInfo,
                        bool &forceHooks) {
  if (!gameBase)
    gameBase = GetGameDllBase();
  if (!gameBase) {
    war3dbg::Print(
        "DXVK War3Hook: 未找到 Game.dll，跳过 Hook（仅使用 DXVK）\n");
    return false;
  }

  if (!GetModuleInfo(reinterpret_cast<HMODULE>(gameBase), gameInfo)) {
    war3dbg::Print("DXVK War3Hook: 解析 Game.dll PE 信息失败，跳过 "
                   "Hook（仅使用 DXVK）\n");
    return false;
  }

  war3dbg::Print(
      "DXVK War3Hook: Game.dll base=%p size=0x%08X timestamp=0x%08X\n",
      reinterpret_cast<void *>(gameInfo.base),
      static_cast<unsigned>(gameInfo.size),
      static_cast<unsigned>(gameInfo.timestamp));

  forceHooks = GetEnvBool("DXVK_WAR3_FORCE_HOOKS", false);
  FileVersion ver = {};
  const bool haveVer =
      QueryModuleFileVersion(reinterpret_cast<HMODULE>(gameInfo.base), ver);
  if (haveVer) {
    war3dbg::Print(
        "DXVK War3Hook: Game.dll fileVersion=%u.%u.%u.%u\n",
        static_cast<unsigned>(ver.major), static_cast<unsigned>(ver.minor),
        static_cast<unsigned>(ver.patch), static_cast<unsigned>(ver.build));
  } else {
    war3dbg::Print("DXVK War3Hook: 无法获取 Game.dll 版本信息\n");
  }

  if (!forceHooks) {
    if (!haveVer || ver.major != 1 || ver.minor != 27) {
      war3dbg::Print(
          "DXVK War3Hook: 当前 Game.dll 非 1.27.x 或版本未知，已跳过 "
          "Hook（仅使用 DXVK）。如确认偏移已适配可设置 "
          "DXVK_WAR3_FORCE_HOOKS=1\n");
      return false;
    }
  } else {
    war3dbg::Print("DXVK War3Hook: 已启用 "
                   "DXVK_WAR3_FORCE_HOOKS，跳过版本检查（风险自负）\n");
  }

  return true;
}

} // namespace

/**
 * @brief 运行时激活入口。
 *
 * 生命周期域 Hook（MainRunner/MainRunner_Alt）在游戏主流程稳定后调用本函数，
 * 统一完成 War3 运行时初始化与完整 Hook 安装。
 */
void ActivateWar3Runtime(uintptr_t gameBase, const char *source) {
  bool expected = false;
  if (!g_war3_runtime_activated.compare_exchange_strong(expected, true)) {
    return;
  }

  war3dbg::Print("DXVK War3Hook: Activation triggered by %s\n", source);

  auto tStart = std::chrono::high_resolution_clock::now();

  dxvk::war3::platform::InitializeRuntimeCore(gameBase);
  auto tInit = std::chrono::high_resolution_clock::now();

  ::war3_preinit();
  auto tPre = std::chrono::high_resolution_clock::now();

  void *rootObj = jass::get_instance(0xD);
  if (rootObj && war3::IsReadableRange((void *)((uintptr_t)rootObj + 0x10),
                                       sizeof(void *))) {
    ::game_table = *(::GameTable **)((uintptr_t)rootObj + 0x10);
    war3dbg::Print("DXVK War3Hook: game_table initialized\n");
  } else {
    war3dbg::Print("DXVK War3Hook: WARN game_table init failed (rootObj=%p)\n",
                   rootObj);
  }
  auto tJass = std::chrono::high_resolution_clock::now();

  auto tNet = std::chrono::high_resolution_clock::now();

  ::war3_init();
  dxvk::war3::War3Events::get().fireOnJassReady();
  war3dbg::Print("DXVK War3Hook: War3 runtime fully initialized\n");
  war3dbg::Print("DXVK War3Hook: JASS runtime fully initialized\n");
  auto tFull = std::chrono::high_resolution_clock::now();

  dxvk::war3::War3Events::get().registerOnGameExit(
      []() { ResetWar3RuntimeState(); });

  // 提前初始化模块系统，避免首帧事件分发时触发额外抖动。
  war3module::InitializeModules();

  War3Hook::InstallGameHooks(gameBase);
  auto tHook = std::chrono::high_resolution_clock::now();

  // 输出运行时总览（只记录一次）。
  dxvk::war3::tools::LogRuntimeSummaryOnce("ActivateWar3Runtime");

  using Ms = std::chrono::duration<double, std::milli>;
  war3dbg::Print("DXVK War3Hook Init Profile:\n"
                 "  Basic Init: %.2f ms\n"
                 "  War3 PreInit: %.2f ms\n"
                 "  Jass Instance: %.2f ms\n"
                 "  NetEvent: %.2f ms\n"
                 "  War3/Japi Init: %.2f ms\n"
                 "  MinHook Install: %.2f ms\n"
                 "  TOTAL: %.2f ms\n",
                 Ms(tInit - tStart).count(), Ms(tPre - tInit).count(),
                 Ms(tJass - tPre).count(), Ms(tNet - tJass).count(),
                 Ms(tFull - tNet).count(), Ms(tHook - tFull).count(),
                 Ms(tHook - tStart).count());
}

void TryInstallShadowHooksEarly(uintptr_t gameBase, const char *source) {
  (void)gameBase;
  if (GetEnvBool("DXVK_WAR3_BOOTSTRAP_MINIMAL", false)) {
    static std::atomic<bool> s_minimalLogged{false};
    bool expected = false;
    if (s_minimalLogged.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      war3dbg::Print(
          "DXVK War3Hook: 启动最小化二分，忽略 early hook 补装 source=%s\n",
          source ? source : "unknown");
    }
    return;
  }
  TryInstallStormBreakerEarly(source);

  // Phase 7.99/7.170：CWidget identity 与 Shadow producer hooks 必须早装。
  // 地图加载阶段会大量触发 widget/stamp/register 写入；如果等
  // ActivateWar3Runtime 之后才装，会错过预置 path blocker / destructible /
  // doodad 静态阴影注册。
  if constexpr (dxvk::war3::internal::kWar3ShadowHookEnabled) {
    static std::atomic<bool> s_earlyAttempted{false};
    bool expected = false;
    if (s_earlyAttempted.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
      if (!EnsureMinHookInitialized()) {
        Logger::info(str::format(
            "DXVK War3Hook: Shadow early MinHook init failed source=", source));
        s_earlyAttempted.store(false, std::memory_order_release);
        return;
      }

      ModuleInfo gameInfo = {};
      if (gameBase == 0u || !GetModuleInfo(reinterpret_cast<HMODULE>(gameBase),
                                           gameInfo)) {
        Logger::info(str::format(
            "DXVK War3Hook: Shadow early skip - bad Game.dll base=0x",
            std::hex, gameBase, std::dec, " source=", source));
        s_earlyAttempted.store(false, std::memory_order_release);
        return;
      }

      const auto& book =
          dxvk::war3::hooks::GetWar3HookAddressBook127a();
      const uintptr_t rva =
          book.widgetRegisterFootprintAndShadowMask;
      if (rva != 0u && gameBase != 0u) {
        const uintptr_t addr = gameBase + rva;
        if (!IsExecutableRange(reinterpret_cast<const void*>(addr), 16) ||
            !IsReadableRange(reinterpret_cast<const void*>(addr), 16)) {
          Logger::info(str::format(
              "DXVK War3Hook: WidgetIdentity early skip - bad addr=0x",
              std::hex, addr, std::dec, " source=", source));
        } else {
          const bool ok = dxvk::war3::hooks::InstallWidgetIdentityHook(
              reinterpret_cast<LPVOID>(addr));
          Logger::info(str::format(
              "DXVK War3Hook: WidgetIdentity early install addr=0x",
              std::hex, addr, std::dec,
              " result=", (ok ? "ok" : "fail"),
              " source=", source));
        }
      }

      const dxvk::war3::hooks::ShadowHookAddresses shadowHooks =
          BuildShadowHookAddresses(gameInfo, source);
      const bool shadowOk = dxvk::war3::hooks::InstallShadowHooks(shadowHooks);
      Logger::info(str::format(
          "DXVK War3Hook: Shadow early producer install result=",
          (shadowOk ? "ok" : "fail"), " source=", source));
    }
  }
}

void War3Hook::InstallGameHooks(uintptr_t gameBase) {
  if (War3Hook::IsHooksInstalled())
    return;

  if (!GetEnvBool("DXVK_WAR3_ENABLE_HOOKS", true)) {
    war3dbg::Print("DXVK War3Hook: 已通过 DXVK_WAR3_ENABLE_HOOKS 禁用 Hook\n");
    War3Hook::MarkHooksInstalled();
    return;
  }

  ModuleInfo gameInfo = {};
  bool forceHooks = false;
  if (!ValidateGameModule(gameBase, gameInfo, forceHooks)) {
    War3Hook::MarkHooksInstalled();
    return;
  }
  const auto &book = dxvk::war3::hooks::GetWar3HookAddressBook127a();

  War3RenderState::SetSkipUi(false);
  War3RenderState::SetDebugRenderMode(War3RenderState::DebugRenderMode::Normal);

  if (!EnsureMinHookInitialized()) {
    const MH_STATUS st = g_minhookInitStatus.load(std::memory_order_relaxed);
    war3dbg::Print("DXVK War3Hook: MinHook 初始化失败, 错误码=%d\n", int(st));
    War3Hook::MarkHooksInstalled();
    return;
  }

  auto resolveCode = [&](uintptr_t rva, const char *name) -> LPVOID {
    const uintptr_t addr = gameInfo.base + rva;
    if (!IsAddressInModule(addr, gameInfo) ||
        !IsExecutableRange(reinterpret_cast<const void *>(addr), kHookProbeSize) ||
        !IsReadableRange(reinterpret_cast<const void *>(addr), kHookProbeSize)) {
      war3dbg::Print("DXVK War3Hook: 跳过 %s (rva=0x%08X addr=%p) - "
                     "地址不可读/不可执行或版本不匹配\n",
                     name, static_cast<unsigned>(rva),
                     reinterpret_cast<void *>(addr));
      return nullptr;
    }
    return reinterpret_cast<LPVOID>(addr);
  };

  auto resolveData = [&](uintptr_t rva, size_t size,
                         const char *name) -> uintptr_t {
    const uintptr_t addr = gameInfo.base + rva;
    if (!IsAddressInModule(addr, gameInfo) ||
        !IsReadableRange(reinterpret_cast<const void *>(addr), size)) {
      war3dbg::Print("DXVK War3Hook: 跳过 %s (rva=0x%08X addr=%p) - "
                     "数据不可读或版本不匹配\n",
                     name, static_cast<unsigned>(rva),
                     reinterpret_cast<void *>(addr));
      return 0;
    }
    return addr;
  };

  // -------------------------------------------------------------------------
  // RenderQueue 全局地址填充（供 Render/Bridge 子模块共享）。
  // -------------------------------------------------------------------------
  const uintptr_t numOfElementsAddr =
      resolveData(book.rqNumOfElements, sizeof(uint32_t),
                  "RenderQueue_NumOfElements");
  const uintptr_t batchArrayAddr =
      resolveData(book.rqBatchArrayPtr, sizeof(void *),
                  "RenderQueue_BatchArrayPtr");
  const uintptr_t numOfTransparentAddr =
      resolveData(book.rqNumOfTransparent, sizeof(uint32_t),
                  "RenderQueue_NumOfTransparent");
  const uintptr_t sortedBatchCountAddr =
      resolveData(book.rqSortedBatchCount, sizeof(uint32_t),
                  "RenderQueue_SortedBatchCount");
  const uintptr_t sortedBatchPtrsAddr =
      resolveData(book.rqSortedBatchPtrs, sizeof(void *) * 10000,
                  "RenderQueue_SortedBatchPtrs");
  const uintptr_t stateOptEnabledAddr =
      resolveData(book.rqStateOptEnabled, sizeof(uint32_t),
                  "RenderQueue_StateOptEnabled");
  const uintptr_t stateCleanupPendingAddr =
      resolveData(book.rqStateCleanupPending, sizeof(uint32_t),
                  "RenderQueue_StateCleanupPending");

  g_numOfElementsPtr = numOfElementsAddr
                           ? reinterpret_cast<uint32_t *>(numOfElementsAddr)
                           : nullptr;
  g_batchArrayPtr =
      batchArrayAddr ? reinterpret_cast<void **>(batchArrayAddr) : nullptr;
  g_numOfTransparentPtr =
      numOfTransparentAddr ? reinterpret_cast<uint32_t *>(numOfTransparentAddr)
                           : nullptr;
  g_sortedBatchCountPtr =
      sortedBatchCountAddr ? reinterpret_cast<uint32_t *>(sortedBatchCountAddr)
                           : nullptr;
  g_sortedBatchPtrs =
      sortedBatchPtrsAddr ? reinterpret_cast<void **>(sortedBatchPtrsAddr)
                          : nullptr;
  g_stateOptEnabledPtr =
      stateOptEnabledAddr ? reinterpret_cast<uint32_t *>(stateOptEnabledAddr)
                          : nullptr;
  g_stateCleanupPendingPtr =
      stateCleanupPendingAddr
          ? reinterpret_cast<uint32_t *>(stateCleanupPendingAddr)
          : nullptr;

  g_renderQueueItemComparator =
      reinterpret_cast<dxvk::war3::reimpl::ItemComparatorFn>(
          resolveCode(book.rqItemComparator, "RenderQueue_ItemComparator"));
  g_renderQueueApplyStateBlock =
      reinterpret_cast<dxvk::war3::reimpl::ApplyStateBlockFn>(
          resolveCode(book.gxApplyStateBlock, "GxDevice_ApplyStateBlock"));
  g_renderQueueStageUpdate =
      reinterpret_cast<dxvk::war3::reimpl::StageUpdateFn>(
          resolveCode(book.rqStageUpdate, "RenderQueue_StageUpdate"));
  g_renderQueueGxCleanup74 = reinterpret_cast<dxvk::war3::reimpl::GxCleanupFn>(
      resolveCode(book.gxCleanup74, "GxDevice_Cleanup74"));
  g_renderQueueGxCleanup78 = reinterpret_cast<dxvk::war3::reimpl::GxCleanupFn>(
      resolveCode(book.gxCleanup78, "GxDevice_Cleanup78"));

  g_renderQueueGlobalsValid.store(g_numOfElementsPtr != nullptr &&
                                      g_batchArrayPtr != nullptr,
                                  std::memory_order_relaxed);

  if (g_renderQueueGlobalsValid.load(std::memory_order_relaxed)) {
    war3dbg::Print(
        "DXVK War3Hook: RenderQueue全局追踪已启用 numPtr=%p batchPtr=%p\n",
        g_numOfElementsPtr, g_batchArrayPtr);
  }

  const uintptr_t handleMgrAddr =
      resolveData(book.handleManager, sizeof(uintptr_t), "HandleManager");
  const uintptr_t gameWar3Addr =
      resolveData(book.gameWar3, sizeof(uintptr_t), "GameWar3");

  dxvk::war3::render::RenderQueueTracker::instance().SetGlobals(
      g_numOfElementsPtr, g_batchArrayPtr);
  dxvk::war3::render::ExecBatchProcessor::SetHandleManagerAddrs(handleMgrAddr,
                                                                gameWar3Addr);

  // -------------------------------------------------------------------------
  // 分域 Hook 安装。
  // -------------------------------------------------------------------------
  if constexpr (dxvk::war3::internal::kWar3UiHookEnabled) {
    dxvk::war3::hooks::InstallUiHooks(gameInfo.base);
  } else {
    war3dbg::Print("DXVK War3Hook: 二分诊断态关闭 UI hook 安装\n");
  }

  if constexpr (dxvk::war3::internal::kWar3RenderHookEnabled) {
    dxvk::war3::hooks::War3HookRender::Install(gameInfo.base);
  } else {
    war3dbg::Print("DXVK War3Hook: 二分诊断态关闭 Render hook 安装\n");
  }

  if constexpr (dxvk::war3::internal::kWar3ShadowHookEnabled) {
    const dxvk::war3::hooks::ShadowHookAddresses shadowHooks =
        BuildShadowHookAddresses(gameInfo, "InstallGameHooks");
    dxvk::war3::hooks::InstallShadowHooks(shadowHooks);

    // Phase 7.98：CWidget 身份链中央 sync Hook。
    // 这是 path blocker / destructible / building / unit 等所有 CWidget 派生类
    // lifecycle 事件的统一入口（30+ caller）。挂在这里就能拿到 widget+0x30
    // 的 rawcode 与对应 jHandle，喂给 RenderObjectRegistry 的兜底查询，让
    // 路径阻断器拦截、描边、Bloom 在 destructible 上也能稳定生效。
    LPVOID widgetRegisterAddr = resolveCode(
        book.widgetRegisterFootprintAndShadowMask,
        "CWidget_RegisterFootprintAndShadowMask");
    Logger::info(str::format(
        "DXVK War3Hook: WidgetIdentity gameBase=0x",
        std::hex, gameInfo.base, std::dec,
        " RVA=0x", std::hex, book.widgetRegisterFootprintAndShadowMask,
        std::dec));
    // Phase 7.99 诊断：install 前后读目标地址前 8 字节，确认 MinHook 真的写了 jmp。
    auto formatBytes = [](LPVOID addr) {
      if (!addr) return std::string("(null)");
      const uint8_t* p = static_cast<const uint8_t*>(addr);
      char buf[64];
      snprintf(buf, sizeof(buf),
               "%02X %02X %02X %02X %02X %02X %02X %02X",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
      return std::string(buf);
    };
    Logger::info(str::format(
        "DXVK War3Hook: WidgetIdentity pre-install bytes@",
        std::hex, reinterpret_cast<uintptr_t>(widgetRegisterAddr),
        std::dec, " = ", formatBytes(widgetRegisterAddr)));
    const bool widgetOk =
        dxvk::war3::hooks::InstallWidgetIdentityHook(widgetRegisterAddr);
    Logger::info(str::format(
        "DXVK War3Hook: WidgetIdentity install addr=0x",
        std::hex, reinterpret_cast<uintptr_t>(widgetRegisterAddr),
        std::dec, " result=", (widgetOk ? "ok" : "fail"),
        " post-install bytes=", formatBytes(widgetRegisterAddr)));
  } else {
    war3dbg::Print("DXVK War3Hook: 二分诊断态关闭 Shadow hook 安装\n");
  }

  if constexpr (dxvk::war3::internal::kWar3ModelHookEnabled) {
    war3::model::Init(gameInfo.base, false);
  } else {
    war3dbg::Print(
        "DXVK War3Hook: 二分诊断态关闭 runtime model hook 初始化\n");
  }

  if constexpr (dxvk::war3::internal::kNativeRendererHookTakeoverEnabled) {
    war3dbg::Print("DXVK War3Hook: Native renderer hook takeover armed for semantic warmup\n");
  } else {
    war3dbg::Print("DXVK War3Hook: Native renderer hook takeover disabled\n");
  }

  War3Hook::MarkHooksInstalled();
}

void War3Hook::InstallHooks(IDirect3DDevice9 *device) {
  s_device = device;
  dxvk::war3::platform::BindNativeShadowDevice(device);

  if (g_bootstrapInstalled.load(std::memory_order_acquire))
    return;

  if (!GetEnvBool("DXVK_WAR3_ENABLE_HOOKS", true)) {
    war3dbg::Print("DXVK War3Hook: 已通过 DXVK_WAR3_ENABLE_HOOKS 禁用 Hook\n");
    g_bootstrapInstalled.store(true, std::memory_order_release);
    return;
  }

  ModuleInfo gameInfo = {};
  bool forceHooks = false;
  if (!ValidateGameModule(GetGameDllBase(), gameInfo, forceHooks)) {
    g_bootstrapInstalled.store(true, std::memory_order_release);
    return;
  }

  if (!EnsureMinHookInitialized()) {
    const MH_STATUS st = g_minhookInitStatus.load(std::memory_order_relaxed);
    war3dbg::Print("DXVK War3Hook: MinHook 初始化失败, 错误码=%d\n", int(st));
    g_bootstrapInstalled.store(true, std::memory_order_release);
    return;
  }

  // 诊断态可完全跳过 Game.dll bootstrap hooks；此时不会建立控制管道，
  // 只用 Present/窗口证据判断游戏自身是否继续启动。默认路径不变。
  const bool bootstrapNoGameHooks =
      GetEnvBool("DXVK_WAR3_BOOTSTRAP_NO_GAME_HOOKS", false);
  if (bootstrapNoGameHooks) {
    war3dbg::Print(
        "DXVK War3Hook: 启动无 Game hook 二分，仅保留 D3D9 图形路径\n");
    g_bootstrapInstalled.store(true, std::memory_order_release);
    return;
  }

  // 最小化模式只保留能建立控制管道的 JASS/MainRunner 入口，用于排除
  // Storm、Shadow producer 与 model provenance 的启动期侵入。
  const bool bootstrapMinimal =
      GetEnvBool("DXVK_WAR3_BOOTSTRAP_MINIMAL", false);
  if (!bootstrapMinimal) {
    // Shadow producer/StormBreaker 越早装越好：优先在首个 D3D9 bootstrap
    // 尝试安装，若此时依赖尚未就绪，再由 MainRunner_ENTER 补一次兜底。
    TryInstallShadowHooksEarly(gameInfo.base, "Bootstrap");
  } else {
    war3dbg::Print(
        "DXVK War3Hook: 启动最小化二分，跳过 Storm/Shadow early hooks\n");
  }

  // Bootstrap 只做“最早期可安全安装”的生命周期/JASS 入口。
  dxvk::war3::hooks::War3HookJass::Install(gameInfo.base);
  dxvk::war3::hooks::War3HookLifecycle::Install(gameInfo.base);
  if constexpr (dxvk::war3::internal::kWar3ModelHookEnabled &&
                dxvk::war3::internal::kShadowRuntimeModelBootstrapHookEnabled) {
    // owner/source provenance 现在已经收敛到“可能发生在 ActivateWar3Runtime
    // 之前的更早创建窗”。bootstrap 这里只提前安装 provenance 必需 hooks，
    // 把 sprite/pose/local-point 这类高侵入 hook 留到 runtime 激活后再装，
    // 避免把整包 model hook 提前到过早生命周期。
    if (!bootstrapMinimal)
      war3::model::Init(gameInfo.base, true);
  }

  g_bootstrapInstalled.store(true, std::memory_order_release);
}

void War3Hook::MaybeInstallNativeRendererTakeover(const char *reason) {
  if constexpr (!dxvk::war3::internal::kNativeRendererHookTakeoverEnabled)
    return;

  bool expected = false;
  if (!s_nativeTakeoverInstallAttempted.compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    return;
  }

  const uintptr_t gameBase = GetGameDllBase();
  if (gameBase == 0u) {
    war3dbg::Print(
        "DXVK War3Hook: Native renderer hook takeover skipped - Game.dll base unavailable reason=%s\n",
        (reason && reason[0]) ? reason : "(unknown)");
    s_nativeTakeoverInstallAttempted.store(false, std::memory_order_relaxed);
    return;
  }

  war3dbg::Print(
      "DXVK War3Hook: Installing native renderer hook takeover after semantic warmup reason=%s base=%p\n",
      (reason && reason[0]) ? reason : "(unknown)",
      reinterpret_cast<void *>(gameBase));
  ::war3::native::InitializeNativeRendererHooks(gameBase);
}

void War3Hook::TriggerShadowReplay() {
  if (!s_hooksInstalled)
    return;

  void *worldPtr = GetCachedWorldPtr();
  if (!worldPtr)
    return;

  using WorldObjectsRenderGroupFn = int(__fastcall *)(void *, void *, int);
  using FlushAndResetFn = int(__cdecl *)();

  const auto worldGroup = reinterpret_cast<WorldObjectsRenderGroupFn>(
      dxvk::war3::hooks::War3HookRender::GetTrampolineWorldObjectsRenderGroup());
  const auto flushAndReset = reinterpret_cast<FlushAndResetFn>(
      dxvk::war3::hooks::War3HookLifecycle::GetTrampolineFlushAndReset());

  SetShadowPass(true);

  if (worldGroup) {
    WAR3_RENDER_LOG("DXVK War3Hook: Replaying Group 0 (Units)\n");
    worldGroup(worldPtr, nullptr, 0);

    WAR3_RENDER_LOG("DXVK War3Hook: Replaying Group 1 (Buildings)\n");
    worldGroup(worldPtr, nullptr, 1);

    WAR3_RENDER_LOG("DXVK War3Hook: Replaying Group 2 (Effects)\n");
    worldGroup(worldPtr, nullptr, 2);

    WAR3_RENDER_LOG("DXVK War3Hook: Replay Groups Done\n");
  }

  if (flushAndReset) {
    WAR3_RENDER_LOG("DXVK War3Hook: Replay FlushAndReset\n");
    flushAndReset();
  }
}

void *War3Hook::GetCachedWorldPtr() {
  return dxvk::war3::state::RenderState::instance().getWorldPointer();
}

void War3Hook::SetShadowPass(bool active) { s_inShadowPass = active; }

bool War3Hook::IsInShadowPass() { return s_inShadowPass; }

void War3Hook::SetCurrentStage(int stage) {
  s_currentStage.store(stage, std::memory_order_relaxed);
}

int War3Hook::GetCurrentStage() {
  return s_currentStage.load(std::memory_order_relaxed);
}

bool War3Hook::IsUiRendering() { return War3RenderState::IsUiPhase(); }

bool War3Hook::ShouldSkipUiDraw() { return War3RenderState::ShouldSkipUi(); }

bool War3Hook::IsHooksInstalled() { return s_hooksInstalled; }

void War3Hook::MarkHooksInstalled() { s_hooksInstalled = true; }

uint32_t War3Hook::GetDebugColorForCurrentStage() {
  if (IsUiRendering()) {
    return 0;
  }

  const int stage = s_currentStage.load(std::memory_order_relaxed);
  switch (stage) {
  case 0:
    return 0xFF8080FF;
  case 1:
    return 0xFF00FF00;
  case 11:
  case 12:
  case 13:
    return 0xFFFF0000;
  default:
    return 0;
  }
}

bool War3Hook::IsDebugColoringEnabled() { return true; }

void War3Hook::PushUiLayer() {
  // 由 War3RenderState 负责 UI 层状态机，保留接口仅作兼容。
}

void War3Hook::PopLayer(RenderLayer /*prev*/) {
  // 由 War3RenderState 负责层恢复，兼容层保持 no-op。
}

IDirect3DDevice9 *War3Hook::GetDevice() { return s_device; }

} // namespace dxvk
