// war3_storm_hook.h
// Storm.dll 内存分配拦截接口（从 StormBreaker 整合）
//
// 策略：
//   - 大块（>= 64 KB）→ TlsfPool_Alloc，使用私有头部做零锁识别
//   - 小块（< 64 KB）→ 放行原生 Storm，不干预
//
// 说明：
//   - Storm.dll 原生头并不是 10 字节。基于 Storm ASM，SMemFree/GetSize/ReAlloc
//     会固定读取 user[-2]/user[-5]/user[-8]/user[-12]/user[-16] 等偏移。
//   - 为了避免“伪装成 Storm 块但头布局不完全一致”导致原生 SMemFree 误解析
//     崩溃，我们对 TLSF 块使用私有头。若某处意外绕过 Hook 直接调用原生
//     Storm 导出，Storm_CheckMemPointer 会先因 magic 不匹配而拒绝该指针，
//     从而避免落入原生堆释放路径。
//
// Hook 安装：使用项目已有的 MinHook 框架（InstallMinHook），不依赖 Detours。

#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>


namespace dxvk::war3::memory {

// ============================================================================
// 初始化与关闭
// ============================================================================

/**
 * @brief 安装 Storm 分配拦截 Hook。
 *
 * 调用前提：TlsfPool_Init() 必须已成功执行。
 * 会自动定位 Storm.dll 并通过 MinHook 拦截 SMemAlloc/Free/Realloc/GetSize。
 *
 * @return 至少安装一个 Hook 成功返回 true。
 */
bool StormHook_Install();

/** @brief 卸载所有 Storm Hook（DLL 卸载时调用）。 */
void StormHook_Uninstall();

/** @return Hook 是否已安装。 */
bool StormHook_IsInstalled();

/**
 * @brief 控制“大块转 TLSF”是否生效。
 *
 * Hook 仍会保持安装；关闭时仅停止把新的原生大块分配/扩容转入 TLSF，
 * 但仍继续识别并处理已托管块的 Free/GetSize/ReAlloc。
 */
void StormHook_SetRedirectEnabled(bool enabled);

/** @return 当前是否允许将原生大块转入 TLSF。 */
bool StormHook_IsRedirectEnabled();

// ============================================================================
// 大块阈值配置
// ============================================================================

/**
 * @brief 设置大块拦截阈值（默认 64 KB）。
 *
 * 超过此阈值的分配会被重定向到 TLSF 池。
 * 不建议低于 64 KB，否则会干扰 Storm 小块池的内部逻辑。
 */
void StormHook_SetLargeBlockThreshold(size_t bytes);
size_t StormHook_GetLargeBlockThreshold();

// ============================================================================
// 状态查询
// ============================================================================

/** @return ptr 是否是被我们管理的大块（地址范围 + 私有头校验，零锁）。 */
bool StormHook_IsOurBlock(void *userPtr);

/** @return 当前托管块数量。 */
size_t StormHook_GetManagedCount();

/** @return 当前托管总字节数。 */
size_t StormHook_GetManagedSize();

/**
 * @brief 读取 Storm.dll 的 g_TotalAllocatedMemory 全局变量。
 *
 * 该变量只增不减——Storm 原生分配每次 +bytes，释放时不减。
 * StormBreaker 的核心价值：大块转 TLSF 后，此计数不再增长，
 * 接近 2GB 时不再崩溃。周期报告中可观测此值是否稳定。
 *
 * @return 当前 Storm 累计分配字节数（0 = Storm.dll 未加载或读取失败）。
 */
size_t StormHook_GetStormNativeAllocated();

/**
 * @brief 打印一次分钟级内存综合报告（内置 60 秒节流）。
 *
 * 报告内容：Storm g_TotalAllocatedMemory、TLSF 池容量/用量、托管块数量。
 * 建议从 PresentEx / BeginFrame 路径每帧调用，内部自动限流。
 */
void StormHook_PrintPeriodicReport();

// ============================================================================
// TLSF 私有头（内部使用，供 .cpp 共享）
// ============================================================================

#pragma pack(push, 1)
/// TLSF 大块私有头。故意不伪装成 Storm 原生头，以避免原生 SMemFree 误解析。
struct StormTlsfHeader {
  uint32_t magic;         ///< StormBreaker 自有块标识
  uint32_t requestedSize; ///< 用户请求大小
  uint32_t sizeCookie;    ///< requestedSize ^ kStormBreakerCookie
  uint16_t headerSize;    ///< 头大小，供一致性校验
  uint16_t rejectTag;     ///< 故意不写 0x6F6D，令原生 Storm 快速拒绝
};
#pragma pack(pop)

static_assert(sizeof(StormTlsfHeader) == 16,
              "StormTlsfHeader 必须保持 16 字节对齐");

constexpr uint32_t kStormBreakerMagic = 0x53425431u;  // 'SBT1'
constexpr uint32_t kStormBreakerCookie = 0x9E3779B9u;
constexpr uint16_t kStormBreakerRejectTag = 0x4253u; // 'SB'

} // namespace dxvk::war3::memory
