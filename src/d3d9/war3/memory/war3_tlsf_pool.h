// war3_tlsf_pool.h
// TLSF 内存池接口（从 StormBreaker 整合，去除独立 Logger/Detours 依赖）
// 用途：为 War3 大块内存分配提供可回收的 TLSF 池，压制 Storm 计数，防止 2GB
// 溢出崩溃。
//
// 架构说明：
//   - Allocate/Free/Realloc：通用 TLSF 接口，用于大块持久分配（Storm 拦截路径）
//   - AllocArenaBlock：为 Shadow Arena 等帧级线性分配器预留连续大块（永不归还）
//   - TrimFreePages：空闲扩展池回收，供低内存压力时调用

#pragma once

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::memory {

// ============================================================================
// 初始化与关闭
// ============================================================================

/**
 * @brief 初始化 TLSF 内存池。
 *
 * 默认配置：64 MB 初始大小，16 MB 扩展粒度，1 GB 上限。
 * 必须在 Storm Hook 安装前调用（DllMain / D3D9 初始化路径）。
 *
 * @return 初始化成功返回 true。
 */
bool TlsfPool_Init();

/**
 * @brief 关闭并释放全部 TLSF 池内存。
 * @note  仅在 DLL 卸载时调用，正常游戏流程不调用。
 */
void TlsfPool_Shutdown();

/** @return 池是否已初始化。 */
bool TlsfPool_IsInitialized();

// ============================================================================
// 通用分配接口（Storm 拦截路径、其他大块需求）
// ============================================================================

void *TlsfPool_Alloc(size_t size);
void *TlsfPool_AllocAligned(size_t size, size_t alignment);
void *TlsfPool_Realloc(void *ptr, size_t newSize);
void TlsfPool_Free(void *ptr);

/** @return ptr 是否来自本池（通过地址范围判断）。 */
bool TlsfPool_IsFromPool(void *ptr);

/** @return ptr 对应的 TLSF 块大小（0 表示不属于本池）。 */
size_t TlsfPool_BlockSize(void *ptr);

// ============================================================================
// Arena 预留接口（Shadow Arena / 帧级线性分配器专用）
// ============================================================================

/**
 * @brief 从 TLSF 池预留一块连续内存，用于帧级线性分配器。
 *
 * 该块不经过 TlsfPool_Free 归还，生命周期与 DLL 同生命周期。
 * 帧内管理由调用方（ShadowArena）负责。
 *
 * @param size      预留大小（建议 4MB 或 8MB）。
 * @param purpose   调试字符串，用于日志标识。
 * @return          连续内存起始指针，失败返回 nullptr。
 */
void *TlsfPool_AllocArenaBlock(size_t size, const char *purpose);

// ============================================================================
// 池管理
// ============================================================================

/**
 * @brief 扫描并归还完全空闲的扩展池页。
 * @note  不影响主池（64 MB 初始块）和 Arena 预留块。
 */
void TlsfPool_TrimFreePages();

// ============================================================================
// 统计与调试
// ============================================================================

struct TlsfPoolStats {
  size_t totalSize;   ///< 池总容量（字节）
  size_t usedSize;    ///< 当前已分配字节数
  size_t freeSize;    ///< 空闲字节数
  size_t peakUsed;    ///< 历史峰值
  size_t allocCount;  ///< 累计分配次数
  size_t freeCount;   ///< 累计释放次数
  size_t extendCount; ///< 扩展池次数
  size_t trimCount;   ///< Trim 操作次数
};

TlsfPoolStats TlsfPool_GetStats();
void TlsfPool_PrintStats();

// ============================================================================
// 线程安全控制
// ============================================================================

/**
 * @brief 禁用内部锁（仅在确认单线程访问时使用，显著减少开销）。
 *
 * War3 的 Storm 分配在游戏主线程执行，Shadow Arena 也在主线程。
 * 若确认无多线程竞争，调用此函数可省去 Spinlock 开销。
 */
void TlsfPool_DisableLock();
void TlsfPool_EnableLock();
bool TlsfPool_IsLockEnabled();

} // namespace dxvk::war3::memory
