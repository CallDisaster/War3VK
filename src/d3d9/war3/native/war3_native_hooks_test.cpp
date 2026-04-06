// war3_native_hooks_test.cpp
// 测试版本：只Hook CWorldFrameWar3::RenderScene，内部调用原始函数指针
// 用于验证Hook机制本身是否正常

#include "../../d3d9_device.h"
#include "../debug/war3_debug.h"
#include "../core/war3_internal_test_config.h"
#include "war3_native_renderer.h"

#include <cstring>
#include <windows.h>

namespace war3 {
namespace native {

// ============================================================================
// 原始函数指针
// ============================================================================

namespace original {
typedef int(__thiscall *CWorld_RenderScene_t)(CWorldFrameWar3 *);

// 原始CWorldFrameWar3::RenderScene函数指针
CWorld_RenderScene_t CWorld_RenderScene_Original = nullptr;
} // namespace original

// ============================================================================
// Hook 状态
// ============================================================================

static bool g_HookInstalled = false;

// ============================================================================
// 内存保护操作
// ============================================================================

static bool SetMemoryProtection(void *address, size_t size, DWORD newProtect) {
  DWORD oldProtect;
  return VirtualProtect(address, size, newProtect, &oldProtect) != 0;
}

static bool WriteJump(void *target, void *destination) {
  if (!target || !destination) {
    return false;
  }

  if (!SetMemoryProtection(target, 5, PAGE_EXECUTE_READWRITE)) {
    return false;
  }

  uint8_t *ptr = (uint8_t *)target;

  ptr[0] = 0xE9; // JMP rel32
  int32_t offset = (int32_t)((uint8_t *)destination - ptr - 5);
  memcpy(ptr + 1, &offset, 4);

  SetMemoryProtection(target, 5, PAGE_EXECUTE_READ);

  return true;
}

// ============================================================================
// 测试Hook实现
// ============================================================================

/**
 * @brief 测试版本的 CWorldFrameWar3::RenderScene Hook
 * 简单的包装函数，内部调用原始函数指针
 */
extern "C" int Test_CWorld_RenderScene(CWorldFrameWar3 *world) {
  WAR3_LOG_INFO("[TestHook] Test_CWorld_RenderScene called, world=%p\n", world);
  
  // 检查原始函数指针是否有效
  if (!original::CWorld_RenderScene_Original) {
    WAR3_LOG_ERROR("[TestHook] Original function pointer is NULL!\n");
    return 0;
  }
  
  WAR3_LOG_INFO("[TestHook] Calling original function...\n");
  
  // 直接调用原始函数
  int result = original::CWorld_RenderScene_Original(world);
  
  WAR3_LOG_INFO("[TestHook] Original function returned: %d\n", result);
  
  return result;
}

// ============================================================================
// Hook 安装函数
// ============================================================================

/**
 * @brief 只安装 CWorldFrameWar3::RenderScene 的Hook
 */
static bool Hook_CWorld_RenderScene_Test(void *baseAddress) {
  // 函数地址：0x6F3681C0
  // RVA: 0x3681C0
  void *target = (uint8_t *)baseAddress + 0x3681C0;

  WAR3_LOG_INFO("[TestHook] Installing CWorldFrameWar3::RenderScene hook at %p\n", target);

  // 保存原始函数指针
  original::CWorld_RenderScene_Original = (original::CWorld_RenderScene_t)target;

  WAR3_LOG_INFO("[TestHook] Original function saved: %p\n", original::CWorld_RenderScene_Original);

  // 写入JMP到我们的测试函数
  bool success = WriteJump(target, (void *)&Test_CWorld_RenderScene);

  if (success) {
    WAR3_LOG_INFO("[TestHook] Hook installed successfully!\n");
  } else {
    WAR3_LOG_ERROR("[TestHook] Failed to install hook!\n");
  }

  return success;
}

/**
 * @brief 安装测试Hook（只Hook CWorldFrameWar3::RenderScene）
 */
extern "C" int InstallNativeRendererHooksTest(void *baseAddress) {
  WAR3_LOG_INFO("[TestHook] ========================================\n");
  WAR3_LOG_INFO("[TestHook] Installing TEST Hook for CWorldFrameWar3::RenderScene\n");
  WAR3_LOG_INFO("[TestHook] Game.dll base address: %p\n", baseAddress);
  WAR3_LOG_INFO("[TestHook] ========================================\n");

  if (g_HookInstalled) {
    WAR3_LOG_WARN("[TestHook] Hook already installed\n");
    return 1;
  }

  if (Hook_CWorld_RenderScene_Test(baseAddress)) {
    g_HookInstalled = true;
    WAR3_LOG_INFO("[TestHook] Test hook installed: 1\n");
    return 1;
  } else {
    WAR3_LOG_ERROR("[TestHook] Failed to install test hook\n");
    return 0;
  }
}

/**
 * @brief 卸载测试Hook
 */
extern "C" int UninstallNativeRendererHooksTest() {
  WAR3_LOG_INFO("[TestHook] Uninstalling test hook (not fully implemented)...\n");
  g_HookInstalled = false;
  return 0;
}

/**
 * @brief 检查测试Hook是否已安装
 */
extern "C" bool IsNativeRendererHookedTest() { return g_HookInstalled; }

/**
 * @brief 在D3D9设备初始化时安装测试Hook
 */
void InitializeNativeRendererHooksTest(dxvk::D3D9DeviceEx *device) {
  // 获取Game.dll基址
  HMODULE gameDll = GetModuleHandleA("Game.dll");
  if (!gameDll) {
    WAR3_LOG_ERROR("[TestHook] Failed to get Game.dll handle\n");
    return;
  }

  void *baseAddress = (void *)gameDll;
  
  WAR3_LOG_INFO("[TestHook] Ready to install test Hook\n");
  
  // 安装测试Hook
  int hookCount = InstallNativeRendererHooksTest(baseAddress);

  WAR3_LOG_INFO("[TestHook] Test hooks installed: %d\n", hookCount);
  WAR3_LOG_INFO("[TestHook] ========================================\n");
}

} // namespace native
} // namespace war3