// war3_shadow_arena.h
// Shadow 帧级 GPU 线性分配器（bump-pointer allocator）
//
// Arena buffer 为 DEVICE_LOCAL，无 CPU 映射。
// 分配结果（ShadowArenaAllocation）仅含 GPU offset + size + storage + info。
// 动态源通过 EmitCs(ctx->copyBuffer) 写入，GPU shadow pass 直接读取，CPU 不介入。
// 帧间轮转（默认 3 帧）避免 GPU 仍在读取上帧数据时被覆写。

#pragma once

#include "../../dxvk/dxvk_buffer.h"

#include <cstddef>
#include <cstdint>


namespace dxvk::war3::memory {

struct ShadowArenaAllocation {
  uint32_t offset = 0;
  uint32_t size = 0;
  Rc<DxvkBuffer> storage;
  DxvkResourceBufferInfo info = {};

  explicit operator bool() const {
    return storage != nullptr && info.buffer != VK_NULL_HANDLE && size != 0u;
  }
};

struct ShadowArenaDiagnostics {
  uint64_t usedBytes = 0u;
  uint64_t residentBytes = 0u;
  uint64_t perGenerationCapacityBytes = 0u;
  uint64_t residentLimitBytes = 0u;
  uint64_t generation = 0u;
  uint64_t submittedSerial = 0u;
  uint64_t completedSerial = 0u;
  uint64_t busyReuseRejectCount = 0u;
  uint64_t overflowCount = 0u;
  uint32_t activeGenerationCount = 0u;
  uint32_t frameIncomplete = 0u;
};

/**
 * @brief 初始化 Shadow Arena 分配器。
 *
 * 创建三个 64 MiB DEVICE_LOCAL 预热页；每个 GPU 代际最多 384 MiB，
 * 总驻留上限 1.125 GiB。无 CPU 映射；写入通过 EmitCs(ctx->copyBuffer) 完成。
 * 在 D3D9 设备创建后调用，不依赖 TLSF 池。
 *
 * @return 成功返回 true。
 */
bool ShadowArena_Init();
bool ShadowArena_IsInitialized();

/**
 * @brief 切换到当前渲染帧对应的 Arena 分区。
 *
 * 优先轮转三个预热代际，但只有 retire fence 已完成的代际才允许清零复用；
 * GPU 落后时会在总驻留上限内增加 spill 代际。
 */
bool ShadowArena_BeginFrame(uint64_t frameSerial, uint64_t completedSerial);

/** Mark the active generation as owned by all shadow work for frameSerial. */
void ShadowArena_EndFrame(uint64_t frameSerial);

/**
 * @brief 在 Arena 中分配当前帧所需内存（极速无锁分配）。
 *
 * 只涉及当前代际的页游标推进。此块内存在该代际的 completion fence
 * 完成之前均不会被覆写。
 *
 * @param size      需要的内存大小。
 * @param alignment 对齐要求（必须为 2 的幂次，默认 16）。
 * @return          分配结果（含 GPU offset/size/storage/info），空间不足时 operator bool() 返回 false。
 */
ShadowArenaAllocation ShadowArena_Alloc(uint32_t size,
                                        uint32_t alignment = 16);

/**
 * @brief 重置分配器游标。
 *
 * 仅供已经由 ShadowArena_BeginFrame 证明可复用的当前代际使用；内部只将
 * 当前页游标置零，不执行 free。
 */
void ShadowArena_Reset();

/**
 * @brief 查询当前帧 Arena 已用字节数。
 * @return 已用字节数。
 */
uint32_t ShadowArena_UsedBytes();
uint32_t ShadowArena_CapacityBytes();
uint64_t ShadowArena_ResidentBytes();
ShadowArenaDiagnostics ShadowArena_QueryDiagnostics();

} // namespace dxvk::war3::memory
