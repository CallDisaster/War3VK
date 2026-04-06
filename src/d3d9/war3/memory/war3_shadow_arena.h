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

/**
 * @brief 初始化 Shadow Arena 分配器。
 *
 * 创建 DEVICE_LOCAL GPU buffer（默认 8MB × 3帧，capture 模式 96MB × 2帧）。
 * 无 CPU 映射；写入通过 EmitCs(ctx->copyBuffer) 完成。
 * 在 D3D9 设备创建后调用，不依赖 TLSF 池。
 *
 * @return 成功返回 true。
 */
bool ShadowArena_Init();
bool ShadowArena_IsInitialized();

/**
 * @brief 切换到当前渲染帧对应的 Arena 分区。
 *
 * 采用与 D3D9DeviceEx::m_war3FrameIndex 一致的三帧轮转，避免 GPU 仍在读取
 * 上一帧数据时被 CPU 覆写。
 */
void ShadowArena_BeginFrame(uint32_t frameIndex);

/**
 * @brief 在 Arena 中分配当前帧所需内存（极速无锁分配）。
 *
 * 只涉及简单的指针偏移加法，开销 < 5ns。
 * 此块内存在下一次调用 ShadowArena_Reset 之前均有效。
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
 * 应当在 GPU Fence 信号确认上帧数据已被消耗后调用（或简单地将 Arena
 * 分为多帧区域轮转）， 内部实现上会将当前 offset 置零，不需要实际执行 free。
 */
void ShadowArena_Reset();

/**
 * @brief 查询当前帧 Arena 已用字节数。
 * @return 已用字节数。
 */
uint32_t ShadowArena_UsedBytes();
uint32_t ShadowArena_CapacityBytes();

} // namespace dxvk::war3::memory
