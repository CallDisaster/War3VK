// war3_native_hooks.cpp
// Hook集成代码：将还原的渲染函数替换游戏原生函数
// 基于魔兽争霸III (1.27.x) 运行态逆向分析

#include "war3_native_hooks.h"
#include "../../d3d9_device.h"
#include "../core/war3_internal_test_config.h"
#include "../debug/war3_debug.h"
#include "war3_native_renderer.h"

#include <cstring>
#include <windows.h>

namespace war3 {
namespace native {

// ============================================================================
// 原始函数地址表
// ============================================================================

namespace original {
// 原始函数指针定义已经在头文件中声明

// 原始函数指针定义
CWorld_RenderScene_t CWorld_RenderScene = nullptr;
RenderWorld_DispatchStage_t RenderWorld_DispatchStage = nullptr;
WorldObjects_RenderGroup_t WorldObjects_RenderGroup = nullptr;
WorldObjectEntry_Render_t WorldObjectEntry_Render = nullptr;
RenderQueue_AddBatch_t RenderQueue_AddBatch = nullptr;
RenderBatch_Submit_t RenderBatch_Submit = nullptr;
RenderQueue_FlushSortedItems_t RenderQueue_FlushSortedItems = nullptr;
RenderQueue_FlushAndReset_t RenderQueue_FlushAndReset = nullptr;
RenderQueue_ItemComparator_t RenderQueue_ItemComparator = nullptr;
RenderQueue_ItemLess_t RenderQueue_ItemLess = nullptr;
RenderQueue_Dispatch_Common_t RenderQueue_Dispatch_Common = nullptr;
RenderQueue_Dispatch_Special_t RenderQueue_Dispatch_Special = nullptr;
RenderBatch_CanEnqueueToMainQueue_t RenderBatch_CanEnqueueToMainQueue = nullptr;
AUCTransparent_AddEntry_t AUCTransparent_AddEntry = nullptr;

StateCleanup_t StateCleanup = nullptr;
RenderCategory_Enable_t RenderCategory_Enable = nullptr;
RenderCategory_Disable_t RenderCategory_Disable = nullptr;
ApplyCategoryMode_t ApplyCategoryMode = nullptr;
CWorld_SetShadowMode_t CWorld_SetShadowMode = nullptr;
CWorld_ToggleGroup1ShadowPass_t CWorld_ToggleGroup1ShadowPass = nullptr;
Terrain_RenderStage_t Terrain_RenderStage = nullptr;
RenderSceneCleanupContext_Flush_t RenderSceneCleanupContext_Flush_Func =
    nullptr;
RenderStage0_PreRenderContext_t RenderStage0_PreRenderContext_Func = nullptr;
CWorld_HasPostProcessPreview_t CWorld_HasPostProcessPreview_Func = nullptr;
CWorld_CommitPostProcessQueue_t CWorld_CommitPostProcessQueue_Func = nullptr;
CWorld_RenderPreviewContext_t CWorld_RenderPreviewContext_Func = nullptr;
CWorld_RenderSelectionManagerStage_t CWorld_RenderSelectionManagerStage_Func =
    nullptr;
CWorld_RenderStage21IndicatorTail_t CWorld_RenderStage21IndicatorTail_Func =
    nullptr;
} // namespace original

// ============================================================================
// Hook 状态
// ============================================================================

struct HookState {
  bool enabled;
  uint32_t hookCount;

  HookState() : enabled(false), hookCount(0) {}
};

static HookState g_HookState;

// ============================================================================
// 内存保护操作
// ============================================================================

/**
 * @brief 修改内存保护权限
 */
static bool SetMemoryProtection(void *address, size_t size, DWORD newProtect) {
  DWORD oldProtect;
  return VirtualProtect(address, size, newProtect, &oldProtect) != 0;
}

/**
 * @brief 写入跳转指令（JMP）
 */
static bool WriteJump(void *target, void *destination) {
  if (!target || !destination) {
    return false;
  }

  // 修改内存保护为可写可执行
  if (!SetMemoryProtection(target, 5, PAGE_EXECUTE_READWRITE)) {
    return false;
  }

  uint8_t *ptr = (uint8_t *)target;

  // 写入 JMP 指令（5字节）
  ptr[0] = 0xE9; // JMP rel32
  int32_t offset = (int32_t)((uint8_t *)destination - ptr - 5);
  memcpy(ptr + 1, &offset, 4);
  FlushInstructionCache(GetCurrentProcess(), target, 5);

  // 恢复内存保护
  SetMemoryProtection(target, 5, PAGE_EXECUTE_READ);

  return true;
}

/**
 * @brief 写入NOP指令
 */
[[maybe_unused]]
static bool WriteNop(void *address, size_t count) {
  if (!address || count == 0) {
    return false;
  }

  if (!SetMemoryProtection(address, count, PAGE_EXECUTE_READWRITE)) {
    return false;
  }

  memset(address, 0x90, count);
  FlushInstructionCache(GetCurrentProcess(), address, count);

  SetMemoryProtection(address, count, PAGE_EXECUTE_READ);

  return true;
}

/**
 * @brief 写入CALL指令
 */
[[maybe_unused]]
static bool WriteCall(void *target, void *destination) {
  if (!target || !destination) {
    return false;
  }

  if (!SetMemoryProtection(target, 5, PAGE_EXECUTE_READWRITE)) {
    return false;
  }

  uint8_t *ptr = (uint8_t *)target;

  // 写入 CALL 指令（5字节）
  ptr[0] = 0xE8; // CALL rel32
  int32_t offset = (int32_t)((uint8_t *)destination - ptr - 5);
  memcpy(ptr + 1, &offset, 4);
  FlushInstructionCache(GetCurrentProcess(), target, 5);

  SetMemoryProtection(target, 5, PAGE_EXECUTE_READ);

  return true;
}

// ... (Functions in between are unchanged, but I need to target the end of file
// for InitializeNativeRendererHooks)

// ============================================================================
// Hook 安装函数
// ============================================================================

/**
 * @brief Hook CWorldFrameWar3::RenderScene
 */
[[maybe_unused]]
static bool Hook_CWorld_RenderScene(void *baseAddress) {
  // 函数地址：0x6F3681C0 (基于文档)
  // RVA: 0x3681C0
  void *target = (uint8_t *)baseAddress + 0x3681C0;

  // 保存原始函数指针
  original::CWorld_RenderScene = (original::CWorld_RenderScene_t)target;

  // 写入JMP到我们的实现
  bool success = WriteJump(target, (void *)&Native_CWorld_RenderScene);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked CWorldFrameWar3::RenderScene @ %p\n",
                  target);
  }

  return success;
}

/**
 * @brief Hook RenderWorld_DispatchStage
 */
[[maybe_unused]]
static bool Hook_RenderWorld_DispatchStage(void *baseAddress) {
  // 函数地址：0x6F363020
  // RVA: 0x363020
  void *target = (uint8_t *)baseAddress + 0x363020;

  original::RenderWorld_DispatchStage =
      (original::RenderWorld_DispatchStage_t)target;

  bool success = WriteJump(target, (void *)&Native_RenderWorld_DispatchStage);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderWorld_DispatchStage @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook WorldObjects_RenderGroup
 */
[[maybe_unused]]
static bool Hook_WorldObjects_RenderGroup(void *baseAddress) {
  // 函数地址：0x6F368E30
  // RVA: 0x368E30
  void *target = (uint8_t *)baseAddress + 0x368E30;

  original::WorldObjects_RenderGroup =
      (original::WorldObjects_RenderGroup_t)target;

  bool success = WriteJump(target, (void *)&Native_WorldObjects_RenderGroup);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked WorldObjects_RenderGroup @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook WorldObjectEntry_Render
 */
[[maybe_unused]]
static bool Hook_WorldObjectEntry_Render(void *baseAddress) {
  // 函数地址：0x6F184EE0
  // RVA: 0x184EE0
  void *target = (uint8_t *)baseAddress + 0x184EE0;

  original::WorldObjectEntry_Render =
      (original::WorldObjectEntry_Render_t)target;

  bool success = WriteJump(target, (void *)&Native_WorldObjectEntry_Render);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked WorldObjectEntry_Render @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook RenderQueue_AddBatch
 */
[[maybe_unused]]
static bool Hook_RenderQueue_AddBatch(void *baseAddress) {
  // 函数地址：0x6F139190
  // RVA: 0x139190
  void *target = (uint8_t *)baseAddress + 0x139190;

  original::RenderQueue_AddBatch = (original::RenderQueue_AddBatch_t)target;

  bool success = WriteJump(target, (void *)&RenderQueue_AddBatch);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderQueue_AddBatch @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook RenderBatch_Submit
 */
[[maybe_unused]]
static bool Hook_RenderBatch_Submit(void *baseAddress) {
  // 函数地址：0x6F1375C0
  // RVA: 0x1375C0
  if (!baseAddress) {
    WAR3_LOG_ERROR("[War3Hook] RenderBatch_Submit: baseAddress is null\n");
    return false;
  }

  void *target = (uint8_t *)baseAddress + 0x1375C0;

  original::RenderBatch_Submit = (original::RenderBatch_Submit_t)target;

  bool success = WriteJump(target, (void *)&RenderBatch_Submit);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderBatch_Submit @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook RenderQueue_FlushSortedItems
 */
[[maybe_unused]]
static bool Hook_RenderQueue_FlushSortedItems(void *baseAddress) {
  // 函数地址：0x6F1380A0
  // RVA: 0x1380A0
  void *target = (uint8_t *)baseAddress + 0x1380A0;

  original::RenderQueue_FlushSortedItems =
      (original::RenderQueue_FlushSortedItems_t)target;

  bool success = WriteJump(target, (void *)&RenderQueue_FlushSortedItems);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderQueue_FlushSortedItems @ %p\n",
                  target);
  }

  return success;
}

/**
 * @brief Hook RenderQueue_FlushAndReset
 */
[[maybe_unused]]
static bool Hook_RenderQueue_FlushAndReset(void *baseAddress) {
  // 函数地址：0x6F139800
  // RVA: 0x139800
  void *target = (uint8_t *)baseAddress + 0x139800;

  original::RenderQueue_FlushAndReset =
      (original::RenderQueue_FlushAndReset_t)target;

  bool success = WriteJump(target, (void *)&RenderQueue_FlushAndReset);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderQueue_FlushAndReset @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook RenderQueue_ItemComparator
 */
[[maybe_unused]]
static bool Hook_RenderQueue_ItemComparator(void *baseAddress) {
  // 函数地址：0x6F1378B0
  // RVA: 0x1378B0
  void *target = (uint8_t *)baseAddress + 0x1378B0;

  original::RenderQueue_ItemComparator =
      (original::RenderQueue_ItemComparator_t)target;

  bool success = WriteJump(target, (void *)&RenderQueue_ItemComparator);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderQueue_ItemComparator @ %p\n",
                  target);
  }

  return success;
}

/**
 * @brief Hook RenderQueue_ItemLess
 */
[[maybe_unused]]
static bool Hook_RenderQueue_ItemLess(void *baseAddress) {
  // 函数地址：0x6F137D50
  // RVA: 0x137D50
  void *target = (uint8_t *)baseAddress + 0x137D50;

  original::RenderQueue_ItemLess = (original::RenderQueue_ItemLess_t)target;

  bool success = WriteJump(target, (void *)&RenderQueue_ItemLess);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderQueue_ItemLess @ %p\n", target);
  }

  return success;
}

/**
 * @brief Hook RenderBatch_CanEnqueueToMainQueue
 */
[[maybe_unused]]
static bool Hook_RenderBatch_CanEnqueueToMainQueue(void *baseAddress) {
  // 函数地址：0x6F1387E0
  // RVA: 0x1387E0
  void *target = (uint8_t *)baseAddress + 0x1387E0;

  original::RenderBatch_CanEnqueueToMainQueue =
      (original::RenderBatch_CanEnqueueToMainQueue_t)target;

  bool success = WriteJump(target, (void *)&RenderBatch_CanEnqueueToMainQueue);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked RenderBatch_CanEnqueueToMainQueue @ %p\n",
                  target);
  }

  return success;
}

/**
 * @brief Hook AUCTransparent_AddEntry
 */
[[maybe_unused]]
static bool Hook_AUCTransparent_AddEntry(void *baseAddress) {
  // 函数地址：0x6F137AF0
  // RVA: 0x137AF0
  void *target = (uint8_t *)baseAddress + 0x137AF0;

  original::AUCTransparent_AddEntry =
      (original::AUCTransparent_AddEntry_t)target;

  bool success = WriteJump(target, (void *)&AUCTransparent_AddEntry);

  if (success) {
    g_HookState.hookCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked AUCTransparent_AddEntry @ %p\n", target);
  }

  return success;
}

// ============================================================================
// Hook 管理函数
// ============================================================================

/**
 * @brief 初始化所有原始函数指针
 */
static void InitializeOriginalPointers(void *baseAddress) {
  original::CWorld_RenderScene =
      (original::CWorld_RenderScene_t)((uint8_t *)baseAddress + 0x3681C0);
  original::RenderWorld_DispatchStage =
      (original::RenderWorld_DispatchStage_t)((uint8_t *)baseAddress +
                                              0x363020);
  original::WorldObjects_RenderGroup =
      (original::WorldObjects_RenderGroup_t)((uint8_t *)baseAddress + 0x368E30);
  original::WorldObjectEntry_Render =
      (original::WorldObjectEntry_Render_t)((uint8_t *)baseAddress + 0x184EE0);
  original::RenderQueue_AddBatch =
      (original::RenderQueue_AddBatch_t)((uint8_t *)baseAddress + 0x139190);
  original::RenderBatch_Submit =
      (original::RenderBatch_Submit_t)((uint8_t *)baseAddress + 0x1375C0);
  original::RenderQueue_FlushSortedItems =
      (original::RenderQueue_FlushSortedItems_t)((uint8_t *)baseAddress +
                                                 0x1380A0);
  original::RenderQueue_FlushAndReset =
      (original::RenderQueue_FlushAndReset_t)((uint8_t *)baseAddress +
                                              0x139800);
  original::RenderQueue_ItemComparator =
      (original::RenderQueue_ItemComparator_t)((uint8_t *)baseAddress +
                                               0x1378B0);
  original::RenderQueue_ItemLess =
      (original::RenderQueue_ItemLess_t)((uint8_t *)baseAddress + 0x137D50);
  original::RenderBatch_CanEnqueueToMainQueue =
      (original::RenderBatch_CanEnqueueToMainQueue_t)((uint8_t *)baseAddress +
                                                      0x1387E0);
  original::AUCTransparent_AddEntry =
      (original::AUCTransparent_AddEntry_t)((uint8_t *)baseAddress + 0x137AF0);

  // 实用工具函数初始化
  original::StateCleanup =
      (original::StateCleanup_t)((uint8_t *)baseAddress + 0x185450);
  original::RenderCategory_Enable =
      (original::RenderCategory_Enable_t)((uint8_t *)baseAddress + 0x0A1400);
  original::RenderCategory_Disable =
      (original::RenderCategory_Disable_t)((uint8_t *)baseAddress + 0x0A2800);
  original::ApplyCategoryMode =
      (original::ApplyCategoryMode_t)((uint8_t *)baseAddress + 0x363350);
  original::CWorld_SetShadowMode =
      (original::CWorld_SetShadowMode_t)((uint8_t *)baseAddress + 0x76F550);
  original::CWorld_ToggleGroup1ShadowPass =
      (original::CWorld_ToggleGroup1ShadowPass_t)((uint8_t *)baseAddress +
                                                  0x3621E0);
  original::Terrain_RenderStage =
      (original::Terrain_RenderStage_t)((uint8_t *)baseAddress + 0x76F060);
  original::RenderSceneCleanupContext_Flush_Func =
      (original::RenderSceneCleanupContext_Flush_t)((uint8_t *)baseAddress +
                                                    0x3ACCF0);
  original::RenderStage0_PreRenderContext_Func =
      (original::RenderStage0_PreRenderContext_t)((uint8_t *)baseAddress +
                                                  0x186300);
  original::CWorld_HasPostProcessPreview_Func =
      (original::CWorld_HasPostProcessPreview_t)((uint8_t *)baseAddress +
                                                 0x3597C0);
  original::CWorld_CommitPostProcessQueue_Func =
      (original::CWorld_CommitPostProcessQueue_t)((uint8_t *)baseAddress +
                                                  0x3ACFF0);
  original::CWorld_RenderPreviewContext_Func =
      (original::CWorld_RenderPreviewContext_t)((uint8_t *)baseAddress +
                                                0x3C4330);
  original::CWorld_RenderSelectionManagerStage_Func =
      (original::CWorld_RenderSelectionManagerStage_t)((uint8_t *)baseAddress +
                                                       0x367980);
  original::CWorld_RenderStage21IndicatorTail_Func =
      (original::CWorld_RenderStage21IndicatorTail_t)((uint8_t *)baseAddress +
                                                      0x76F190);
}

/**
 * @brief 安装所有渲染Hook
 * @param baseAddress Game.dll基址（运行时动态获取）
 * @return 成功安装的Hook数量
 */
extern "C" int InstallNativeRendererHooks(void *baseAddress) {
  // 检查是否启用Hook接管 (原生测试优先)
  if (!dxvk::war3::internal::kNativeRendererHookTakeoverEnabled) {
    if (dxvk::war3::internal::kNativeRendererHookVerboseLogging) {
      WAR3_LOG_INFO(
          "[War3Hook] Native Renderer Hook takeover is disabled by config\n");
    }
    return 0;
  }

  // 提示：原生测试模式已激活
  if (dxvk::war3::internal::kNativeRendererHookVerboseLogging) {
    WAR3_LOG_INFO("[War3Hook] Native Test Mode Active: Prioritizing Native "
                  "Hooks over existing ones.\n");
  }

  if (g_HookState.enabled) {
    if (dxvk::war3::internal::kNativeRendererHookVerboseLogging) {
      WAR3_LOG_INFO("[War3Hook] Hooks already installed\n");
    }
    return g_HookState.hookCount;
  }

  if (dxvk::war3::internal::kNativeRendererHookVerboseLogging) {
    WAR3_LOG_INFO("[War3Hook] Installing native renderer hooks...\n");
    WAR3_LOG_INFO("[War3Hook] Game.dll base address: %p\n", baseAddress);
  }

  // 第一步：初始化所有原始指针
  InitializeOriginalPointers(baseAddress);

  int successCount = 0;

  // 此时我们只真正 Hook CWorldFrameWar3::RenderScene
  void *renderSceneTarget = (uint8_t *)baseAddress + 0x3681C0;
  if (WriteJump(renderSceneTarget, (void *)&Native_CWorld_RenderScene)) {
    successCount++;
    WAR3_LOG_INFO("[War3Hook] Hooked CWorldFrameWar3::RenderScene @ %p\n",
                  renderSceneTarget);
  }

  // 暂时不 Hook 其他函数，让 Native_CWorld_RenderScene 调用 original:: 指针
  g_HookState.hookCount = 1;

  if ((uint32_t)successCount == g_HookState.hookCount) {
    g_HookState.enabled = true;
    if (dxvk::war3::internal::kNativeRendererHookVerboseLogging) {
      WAR3_LOG_INFO("[War3Hook] Successfully installed %d hooks\n",
                    g_HookState.hookCount);
    }
  } else {
    WAR3_LOG_ERROR("[War3Hook] ERROR: Failed to install some hooks (%d/%d)\n",
                   successCount, g_HookState.hookCount);

    if (!dxvk::war3::internal::kNativeRendererHookContinueOnError) {
      WAR3_LOG_ERROR("[War3Hook] FATAL: Hook installation failed and "
                     "continue-on-error is disabled\n");
      WAR3_LOG_ERROR(
          "[War3Hook] The game may crash or have rendering issues\n");
      return -1;
    } else {
      WAR3_LOG_WARN(
          "[War3Hook] WARNING: Continuing with partial hooks (%d installed)\n",
          successCount);
      g_HookState.enabled = true; // 部分Hook仍然可用
    }
  }

  return g_HookState.hookCount;
}

/**
 * @brief 卸载所有渲染Hook
 * @return 返回值
 *
 * 注意：卸载Hook比较复杂，需要恢复原始代码。
 * 这个功能目前仅用于调试，实际使用中不建议卸载。
 */
extern "C" int UninstallNativeRendererHooks() {
  if (!g_HookState.enabled) {
    WAR3_LOG_WARN("[War3Hook] Hooks not installed\n");
    return 0;
  }

  WAR3_LOG_INFO("[War3Hook] Uninstalling hooks (not fully implemented)...\n");

  // TODO: 实现完整的卸载逻辑
  // 需要保存原始代码并在卸载时恢复

  g_HookState.enabled = false;
  return 0;
}

/**
 * @brief 检查Hook是否已安装
 */
extern "C" bool IsNativeRendererHooked() { return g_HookState.enabled; }

/**
 * @brief 获取已安装的Hook数量
 */
extern "C" int GetInstalledHookCount() { return g_HookState.hookCount; }

// ============================================================================
// D3D9设备集成
// ============================================================================

/**
 * @brief 在D3D9设备初始化时安装Hook
 */
void InitializeNativeRendererHooks(uintptr_t gameBase) {
  void *baseAddress = reinterpret_cast<void *>(gameBase);
  if (!baseAddress) {
    HMODULE gameDll = GetModuleHandleA("Game.dll");
    if (!gameDll) {
      WAR3_LOG_ERROR("[War3Hook] Failed to get Game.dll handle\n");
      return;
    }

    baseAddress = reinterpret_cast<void *>(gameDll);
  }

  WAR3_LOG_INFO("[War3Hook] Ready to install native renderer Hooks base=%p\n",
                baseAddress);
  // 安装Hook
  int hookCount = InstallNativeRendererHooks(baseAddress);

  WAR3_LOG_INFO("[War3Hook] Native renderer hooks installed: %d\n", hookCount);
  WAR3_LOG_INFO("[War3Hook] Ready to intercept rendering calls\n");
}

} // namespace native
} // namespace war3
