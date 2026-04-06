#pragma once

// War3 原生渲染队列复现 - 数据结构
// 说明：本目录的结构体与偏移以 Game.dll 1.27.x（32位）为准

#include <cstdint>
#include <d3d9.h>

namespace dxvk {
namespace war3 {
namespace reimpl {

// {4B69F6DA-58B2-4A8B-8367-3F7A4D0F8E3C}
static const GUID GUID_War3InstancedShader = {
    0x4b69f6da,
    0x58b2,
    0x4a8b,
    {0x83, 0x67, 0x3f, 0x7a, 0x4d, 0x0f, 0x8e, 0x3c}};

// ============================================================================
// 全局 RenderQueue 变量（RVA 偏移，需加上 Game.dll 基址）
// ============================================================================
// g_RenderQueue_NumOfElements: RVA 0xBC6BAC
// g_RenderQueue_BatchArray:    RVA 0xBC6BB0
// g_RenderQueue_SecondaryCounter: RVA 0xBC6BBC

// [REMOVED] Batch tag moved to global dxvk namespace in war3_render_state.h

struct RenderBatchElement {

  void *renderablePart; // +0x00: RenderablePart*
  uint32_t flags;       // +0x04: flags&3==3 => Dispatch_Special(Type3)
  uint32_t layerIndex;  // +0x08: 网格层索引
  uint32_t subIndex;   // +0x0C: 在该 RenderablePart 下的可见层序号 (0, 1, 2...)
  void *layerStatePtr; // +0x10: 层状态块指针 (LayerStateBlock*)
};
#if defined(_M_IX86) || defined(__i386__)
static_assert(sizeof(RenderBatchElement) == 20,
              "RenderBatchElement 大小不匹配");
#endif

// ============================================================================
// List Container (游戏内部的链表容器)
// ============================================================================
struct ListContainer {
  uint32_t reserved[3]; // +0x00: 未知
  void *data;           // +0x0C (+3*4): 数据指针
  uint32_t reserved2;   // +0x10: 未知
  uint32_t count;       // +0x14 (+5*4): 元素数量
};

// ============================================================================
// World Object List Entry (24 bytes / 6 DWORDs)
// ============================================================================
struct WorldObjectListEntry {
  void *worldObjectEntry; // +0x00: WorldObjectEntry*
  uint32_t reserved[4];   // +0x04~0x13: 未知
  uint32_t handleOrUnit;  // +0x14: CUnit* 或 HandleId
};
#if defined(_M_IX86) || defined(__i386__)
static_assert(sizeof(WorldObjectListEntry) == 24,
              "WorldObjectListEntry 大小不匹配");
#endif

// ============================================================================
// Mesh Layer Data (44 bytes / 11 DWORDs)
// 每个对象有多个网格层, 每层单独提交渲染
// ============================================================================
struct MeshLayerData {
  void *materialPtr; // +0x00: 材质指针
  uint32_t data[10]; // +0x04~0x2B: 其他数据
};
#if defined(_M_IX86) || defined(__i386__)
static_assert(sizeof(MeshLayerData) == 44, "MeshLayerData 大小不匹配");
#endif

// ============================================================================
// Layer State Block (36 bytes stride in layer iteration)
// ============================================================================
struct LayerStateBlock {
  uint32_t data[9]; // 9 DWORD = 36 bytes
  // 备注：IDA 反编译中会读取 +0x18 的 DWORD（data[6]）用于“主队列 vs
  // 透明分流”的判定。
};

// ============================================================================
// World Object (偏移量基于逆向分析)
// ============================================================================
// this[91] = WorldObjects 列表 (单位/可破坏物)
// this[92] = SelectionOverlay 列表 (选择圈)
// this[93] = Decorations 列表 (装饰物/特效)
constexpr int WORLD_OBJECT_LIST_OFFSET = 91;
constexpr int WORLD_OVERLAY_LIST_OFFSET = 92;
constexpr int WORLD_DECORATION_LIST_OFFSET = 93;

} // namespace reimpl
} // namespace war3
} // namespace dxvk
