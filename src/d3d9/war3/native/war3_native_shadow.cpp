// war3_native_shadow.cpp
// 地形阴影链路完整逆向还原（Game.dll 1.27.x）
//
// 注意：
// - 本文件以“结构语义+流程”为主要目标，便于与 IDA 对照。
// - 关键调用点保留为占位函数（避免引入真实依赖）。
// - 若需要运行态接管，可将占位函数替换为 Game.dll 地址调用。

#include "war3_native_shadow.h"

#include <cmath>
#include <cstdint>

namespace war3 {
namespace native {

// ============================================================================
// 占位函数（对应 Game.dll 内部调用）
// ============================================================================

namespace {

// 0x6F0E38E0: GxDevice_ResetSecondaryResource
static inline void GX_ResetState0() {}

// 0x6F0E38D0: GxDevice_BindPrimaryResource
static inline void GX_ResetState1(int /*mode*/) {}

// 0x6F704970: 应用阴影纹理状态块（内部会调用 GxDevice_ApplyStateBlock）
static inline void Shadow_ApplyLayerState(void* /*stateBlock*/, int /*stage*/) {}

// 0x6F705090: 绑定纹理并统计（带计数器）
static inline void Shadow_SubmitTexture(uint32_t /*tex*/, int /*stage*/,
                                        int /*flag*/, int /*idx*/, int /*extra*/) {}

// 0x6F0E35B0: 绘制调用（实际是 gx_device->vtable[0x68]）
static inline void GX_DrawShadowPrimitive(void* /*state0*/, void* /*state1*/,
                                          void* /*vb*/, void* /*ib*/,
                                          int /*primCount*/) {}

// 0x6F0E3520: DrawPrimitive (另一条路径)
static inline void GX_DrawPrimitive(int /*stage*/, uint32_t /*tex*/,
                                    uint32_t /*count*/, uint32_t /*stride*/) {}

// 0x6F76A490: 查找投影器模板
static inline void* sub_6F76A490(int /*id*/) { return nullptr; }

// 0x6F763420: 创建投影器模板
static inline int sub_6F763420() { return 0; }

// 0x6F050D00: 生成回调/纹理句柄
static inline int sub_6F050D00(void* /*ptr*/) { return 0; }

// 0x6F771060: 获取地形对象
static inline int GetTerrain_771060() { return 0; }

// 0x6F7276D0: 初始化投影器池
static inline void sub_6F7276D0(int /*terrain*/) {}

// 0x6F713CA0: 投影器加入更新链
static inline int sub_6F713CA0(int /*a1*/, int /*a2*/, int /*a3*/, int /*a4*/,
                               int /*a5*/, int /*a6*/, int /*a7*/, int /*a8*/,
                               int /*a9*/) {
  return -1;
}

// 0x6F725F70: ShadowTypeInfo 查表
static inline ShadowTypeInfo* Shadow_GetTypeInfo(void* tableBase,
                                                  uint32_t typeId) {
  if (!tableBase)
    return nullptr;
  return reinterpret_cast<ShadowTypeInfo*>(
      reinterpret_cast<uint8_t*>(tableBase) + typeId * 0x4C);
}

// 0x6F73DE40: 读取 flags_970 bit6
static inline int Shadow_GetAltModeFlag(TerrainShadowLayer* layer) {
  if (!layer)
    return 0;
  return (layer->flags_970 & 0x8000u) ? 0 : ((layer->flags_970 >> 6) & 1);
}

// 0x6F73DE20: 读取 flags_970 bit0 / bit15
static inline int Shadow_IsLayerEnabled(TerrainShadowLayer* layer) {
  if (!layer)
    return 0;
  if (layer->flags_970 & 0x8000u)
    return 1;
  return (layer->flags_970 & 1u) ? 1 : 0;
}

// 0x6F737470: 检查 ShadowTypeInfo 是否可用
static inline int Shadow_TypeReady(void* tableBase, uint32_t typeId) {
  auto* info = Shadow_GetTypeInfo(tableBase, typeId);
  if (!info)
    return 0;
  if (!info->stateBlock0)
    return 0;
  if (!info->stateBlock1)
    return 0;
  return 1;
}

} // namespace

// ============================================================================
// 公开函数实现（按 IDA 流程还原）
// ============================================================================

int Terrain_Shadow_IsLayerEnabled(TerrainShadowLayer* layer) {
  return Shadow_IsLayerEnabled(layer);
}

int Terrain_Shadow_GetAltMode(TerrainShadowLayer* layer) {
  return Shadow_GetAltModeFlag(layer);
}

// 0x6F737620
void Terrain_Shadow_RenderLayer(TerrainShadowLayer* layer, int a2, int a3,
                                int a4) {
  if (!layer)
    return;
  if (!(layer->flags_970 & 0x80u))
    return;
  if (!Shadow_IsLayerEnabled(layer))
    return;

  // ListA
  if (a2 != 0) {
    if (layer->listA_count > 0 && layer->listA_ptrs) {
      for (uint32_t i = 0; i < layer->listA_count; i++) {
        auto* entry = reinterpret_cast<ShadowListAEntry*>(layer->listA_ptrs[i]);
        Terrain_Shadow_RenderListA(layer, entry);
      }
    }
  }

  // ListB
  if (a3 != 0) {
    if (layer->flags_970 & 0x100u) {
      Terrain_Shadow_RenderListB(layer, 0, a4);
    }
  }
}

// 0x6F737500
int Terrain_Shadow_RenderListA(TerrainShadowLayer* layer,
                               ShadowListAEntry* entry) {
  if (!layer || !entry)
    return 0;
  if (entry->typeId == 0xFFFFFFFFu)
    return 0;

  // 查表
  ShadowTypeInfo* info =
      Shadow_GetTypeInfo(layer->shadowTypeTable, entry->typeId);
  if (!info)
    return 0;

  GX_ResetState0();
  GX_ResetState1(2);

  // 绑定纹理状态（entry->layerTex->+8）
  if (entry->layerTex) {
    void* texState = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(entry->layerTex) + 0x08);
    Shadow_ApplyLayerState(texState, 2);
  }

  // 单纹理块（blobPtr != 0）
  if (entry->blobPtr) {
    GX_DrawShadowPrimitive(info->stateBlock0, info->stateBlock1, info->vbOrMesh1,
                           info->vbOrMesh0, 8);
    Shadow_SubmitTexture(entry->blobArg, 2, 0, 0, 0xFFFFFFFFu);
  }

  // 组列表渲染（groupListB）
  if (entry->groupCountB && entry->groupListB) {
    auto* group =
        reinterpret_cast<ShadowListAGroupHeader*>(entry->groupListB);
    for (uint32_t i = 0; i < entry->groupCountB; i++) {
      ShadowListAGroupNode* node = group->head;
      while (node) {
        GX_DrawShadowPrimitive(node->stateBlockA, node->stateBlockB,
                               node->meshPtr, node->vbPtr, 8);
        if (node->childCount && node->childList) {
          auto* child = reinterpret_cast<ShadowListAChild*>(node->childList);
          for (uint32_t c = 0; c < node->childCount; c++) {
            Shadow_SubmitTexture(child->texHandle, 2, 0, 0, 0xFFFFFFFFu);
            child = reinterpret_cast<ShadowListAChild*>(
                reinterpret_cast<uint8_t*>(child) + 0x18);
          }
        }
        node = node->next;
      }
      group = reinterpret_cast<ShadowListAGroupHeader*>(
          reinterpret_cast<uint8_t*>(group) + 0x24);
    }
  }

  return 1;
}

// 0x6F737400
void Terrain_Shadow_RenderListB(TerrainShadowLayer* layer, int idFilter,
                                int passType) {
  if (!layer || !layer->listB_ptr || layer->listB_count == 0)
    return;

  auto* entry = reinterpret_cast<ShadowListBEntry*>(layer->listB_ptr);
  uint32_t count = layer->listB_count;

  for (uint32_t i = 0; i < count; i++) {
    // flags[0] 必须有 bit0
    if ((entry->flags & 1) != 0) {
      if (entry->id == static_cast<uint32_t>(idFilter)) {
        // passType 过滤 0x200
        if (passType == 1) {
          if (entry->flags & 0x200) {
            entry = reinterpret_cast<ShadowListBEntry*>(
                reinterpret_cast<uint8_t*>(entry) + 0xA0);
            continue;
          }
        } else if (passType == 2) {
          if (!(entry->flags & 0x200)) {
            entry = reinterpret_cast<ShadowListBEntry*>(
                reinterpret_cast<uint8_t*>(entry) + 0xA0);
            continue;
          }
        }

        // 0x40 / 0x20 / 0x4 的组合过滤
        if ((entry->flags & 0x40) || (entry->flags & 0x20)) {
          if (entry->flags & 0x4) {
            Terrain_Shadow_RenderListBEntry(layer, entry);
          }
        }
      }
    }

    entry = reinterpret_cast<ShadowListBEntry*>(
        reinterpret_cast<uint8_t*>(entry) + 0xA0);
  }
}

// 0x6F737310
int Terrain_Shadow_RenderListBEntry(TerrainShadowLayer* layer,
                                    ShadowListBEntry* entry) {
  if (!layer || !entry)
    return 0;

  GX_ResetState0();
  GX_ResetState1(3);

  // 选择状态块
  void* stateBlock = nullptr;
  if (entry->slotCompare == entry->slotA) {
    stateBlock = entry->altBlock;
  } else {
    // sub_6F7261D0: 根据 entry->slotCompare 选择
    stateBlock = entry->altBlock; // 语义占位
  }

  // 绘制主体
  GX_DrawShadowPrimitive(stateBlock, nullptr,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(entry->texA)),
                         reinterpret_cast<void*>(static_cast<uintptr_t>(entry->vbCountA)), 8);

  // 绑定纹理/计数
  Shadow_SubmitTexture(entry->id, 2, 0, 0, 0xFFFFFFFFu);
  GX_DrawPrimitive(4, entry->texB, entry->texC, 0);

  return 1;
}

// 0x6F7377B0
int Terrain_Shadow_RenderGroupEntry(TerrainShadowLayer* layer,
                                    ShadowListAGroupHeader* groupHeader,
                                    int stateId, int useAltState) {
  if (!layer || !groupHeader)
    return 0;

  // groupHeader->head 的 entryId 位于 +0x18
  auto* node = groupHeader->head;
  if (!node)
    return 0;

  // 查表
  uint32_t typeId =
      *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(groupHeader) +
                                    0x18);
  ShadowTypeInfo* info =
      Shadow_GetTypeInfo(layer->shadowTypeTable, typeId);
  if (!info || !info->stateBlock0)
    return 0;

  if (useAltState) {
    Shadow_ApplyLayerState(reinterpret_cast<void*>(stateId), 0);
  }

  // 只有 ShadowTypeInfo 就绪才绘制
  if (!Shadow_TypeReady(layer->shadowTypeTable, typeId))
    return 0;

  GX_DrawShadowPrimitive(info->stateBlock0, info->stateBlock1, info->vbOrMesh1,
                         info->vbOrMesh0, 8);

  // 使用 shadowMode 选择纹理
  int texMode = Shadow_GetAltModeFlag(layer);
  Shadow_SubmitTexture(info->texA, 2, 0, texMode, 0);
  return 1;
}

// 0x6F737860
void Terrain_Shadow_RenderGroups(TerrainShadowLayer* layer,
                                 ShadowListAEntry* entry) {
  if (!layer || !entry)
    return;

  // 预处理
  Terrain_Shadow_PrepareListA(layer);

  // 遍历 entry->groupListA
  if (entry->groupCountA && entry->groupListA) {
    auto* group =
        reinterpret_cast<ShadowListAGroupHeader*>(entry->groupListA);
    for (uint32_t i = 0; i < entry->groupCountA; i++) {
      int useAlt = (i == 0) ? 2 : 0;
      Terrain_Shadow_RenderGroupEntry(layer, group, useAlt, 1);
      group = reinterpret_cast<ShadowListAGroupHeader*>(
          reinterpret_cast<uint8_t*>(group) + 0x1C);
    }
  }

  if (layer->flags_970 & 0x400u) {
    auto* group =
        reinterpret_cast<ShadowListAGroupHeader*>(entry->groupListA);
    Terrain_Shadow_RenderGroupEntry(layer, group, 2, 1);
  }
}

// 0x6F7376E0
void Terrain_Shadow_PrepareListA(TerrainShadowLayer* layer) {
  if (!layer)
    return;
  if (!Shadow_IsLayerEnabled(layer))
    return;

  GX_ResetState0();
  GX_ResetState1(2);

  // dword_6FB6A624 != 0 时走另一条分支（此处只保留主流程）
  if (layer->listA_count && layer->listA_ptrs) {
    for (uint32_t i = 0; i < layer->listA_count; i++) {
      auto* entry = reinterpret_cast<ShadowListAEntry*>(layer->listA_ptrs[i]);
      Terrain_Shadow_RenderGroups(layer, entry);
    }
  }
}

// 0x6F73DC00
void Terrain_Shadow_InitGrid(TerrainShadowLayer* layer) {
  if (!layer)
    return;

  // gridX/Y = ((size-1) >> 2) + 1
  uint32_t gridX = (layer->gridWidth - 1) >> 2;
  uint32_t gridY = (layer->gridHeight - 1) >> 2;
  gridX++;
  gridY++;

  // entries = [layer+0x104], stride=0x94
  auto* entryBase = *reinterpret_cast<uint8_t**>(
      reinterpret_cast<uint8_t*>(layer) + 0x104);

  for (uint32_t y = 0; y < gridY; y++) {
    for (uint32_t x = 0; x < gridX; x++) {
      auto* entry = reinterpret_cast<ShadowListAEntry*>(
          entryBase + (x + y * gridX) * 0x94);
      Terrain_Shadow_InitEntry(layer, entry, x, y);
    }
  }
}

// 0x6F73D9F0（核心初始化逻辑，保留关键字段）
void Terrain_Shadow_InitEntry(TerrainShadowLayer* layer,
                              ShadowListAEntry* entry, int gridX, int gridY) {
  if (!layer || !entry)
    return;

  entry->gridX = gridX;
  entry->gridY = gridY;

  // 清空组列表
  entry->groupCountB = 0;
  entry->groupListB = nullptr;
  entry->cachedResource = nullptr;

  // 关键状态重置
  entry->typeId = 0xFFFFFFFFu;
  entry->flagsWord = 0x8000u;
  entry->flagsByte = 0;
  entry->ownerTerrain = layer;

  // 其余字段由原版函数计算（位置/纹理/缓存等）
}

// 0x6F73DF20
void Terrain_Shadow_MarkEntryDirty(void* shadowUpdateList, int index,
                                   int payload) {
  if (!shadowUpdateList)
    return;

  // 注意：该函数实际依赖 sub_6F722A60 进行索引映射，
  // 这里只保留结构写入的语义说明。
  uint8_t* base = reinterpret_cast<uint8_t*>(shadowUpdateList);
  uint8_t* entry = base + index * 0x78;
  *reinterpret_cast<uint32_t*>(entry + 0x5C) = 1;
  *reinterpret_cast<uint32_t*>(entry + 0x60) = payload;
  *reinterpret_cast<uint32_t*>(entry + 0x64) =
      *reinterpret_cast<uint32_t*>(base + 0x10);
}

// 0x6F73FA20
void Shadow_UpdateList_Run(ShadowUpdateList* list, float deltaTime) {
  if (!list || list->count == 0 || !list->list)
    return;

  // timeAccum += deltaTime * 1000.0
  list->timeAccum += static_cast<uint32_t>(deltaTime * 1000.0f);

  // 遍历 list->count，每个 ShadowUpdateEntry 大小 0x78
  // 满足条件则调用 Shadow_UpdateEntry_Write
  for (uint32_t i = 0; i < list->count; i++) {
    auto* entry = reinterpret_cast<ShadowUpdateEntry*>(
        reinterpret_cast<uint8_t*>(list->list) + i * 0x78);
    if (!entry->active)
      continue;

    uint32_t lastTick = entry->lastTick;
    uint32_t interval = entry->intervalTick;

    if (entry->intervalTick == 0 || entry->lastWrite <= entry->lastTick ||
        list->timeAccum - entry->lastWrite >= entry->intervalTick) {
      float strength = 1.0f;
      if (entry->interval) {
        float t = static_cast<float>(list->timeAccum - lastTick) /
                  static_cast<float>(entry->interval);
        strength = t > 1.0f ? 1.0f : t;

        if (strength > 1.0f) {
          int ticks = entry->lifeNow;
          while (strength > 1.0f) {
            strength -= 1.0f;
            ++ticks;
          }
          entry->lifeNow = ticks;
          entry->lastTick = list->timeAccum -
                            static_cast<uint32_t>(entry->interval * strength);
        }
      }

      int removeAfterWrite = 0;
      if (entry->flags0C == 0 && entry->lifeNow >= entry->lifeMax) {
        removeAfterWrite = 1;
        strength = 1.0f;
      }

      Shadow_UpdateEntry_Write(list, entry, strength);
      entry->lastWrite = list->timeAccum;
      if (removeAfterWrite) {
        // sub_6F7232A0(entry)
      }
    }
  }
}

// 0x6F73F7A0
int Shadow_UpdateEntry_Write(ShadowUpdateList* list, ShadowUpdateEntry* entry,
                             float strength) {
  if (!list || !entry || !entry->gridValues || !list->ownerTerrain)
    return 0;

  auto* terrain = list->ownerTerrain;
  if (!terrain->packedGrid)
    return 0;

  const int startX = entry->boundMinX;
  const int startY = entry->boundMinY;
  const int endX = entry->boundMaxX;
  const int endY = entry->boundMaxY;
  const int rowStride = (terrain->gridWidth + 1);

  for (int y = startY; y <= endY; ++y) {
    for (int x = startX; x <= endX; ++x) {
      const uint32_t packed = terrain->packedGrid[7 * (x + y * rowStride)];

      if (entry->useRadius) {
        const float dx = (static_cast<float>(packed & 0x7FC000) - 98304.0f) -
                          entry->centerX;
        const float dy =
            (static_cast<float>((packed >> 9) & 0x7FC000) - 98304.0f) -
            entry->centerY;
        if ((dx * dx + dy * dy) > entry->radiusSq) {
          continue;
        }
      }

      float u = static_cast<float>(entry->cbArgA + strength) /
                static_cast<float>(entry->cbArgB);
      (void)u;
      using CallbackFn = float(__thiscall*)(uint64_t*);
      CallbackFn callback = reinterpret_cast<CallbackFn>(entry->callback);

      uint64_t packedVec = 0;
      if (callback) {
        packedVec = (static_cast<uint64_t>(packed & 0x3FFF | 0x22C000) << 9) |
                    (static_cast<uint64_t>(packed >> 9) & 0x7FC000u) << 32;
      }

      float value = callback ? callback(&packedVec) : 0.0f;
      if (entry->fadeEnabled) {
        value *= (1.0f - (strength > 1.0f ? 1.0f : strength));
      }

      const int writeIndex = x + (entry->boundMaxY - entry->boundMinY + 1) *
                                     (y - entry->boundMinY);
      entry->gridValues[writeIndex] = value;
    }
  }

  // sub_6F740E00(&entry->boundMinX)
  return 1;
}

// 0x6F70CBA0
ShadowProjectorManager* Shadow_ProjectorManager_Init(
    ShadowProjectorManager* mgr, TerrainShadowLayer* owner) {
  if (!mgr)
    return nullptr;
  mgr->ownerLayer = owner;
  mgr->projectorCap = 0;
  mgr->projectorCount = 0;
  mgr->projectors = nullptr;
  mgr->projectorGrow = 0;
  mgr->freeCap = 0;
  mgr->freeCount = 0;
  mgr->freeIndices = nullptr;
  mgr->freeGrow = 0;
  return mgr;
}

// 0x6F70CB70
ShadowProjector* Shadow_Projector_Reset(ShadowProjector* entry) {
  if (!entry)
    return nullptr;
  entry->gridMinX = 0;
  entry->gridMinY = 0;
  entry->gridMaxX = 0;
  entry->gridMaxY = 0;
  return entry;
}

// 0x6F7276D0
ShadowProjectorManager* Shadow_ProjectorManager_GetOrCreate(
    TerrainShadowLayer* layer) {
  if (!layer)
    return nullptr;
  // 原版逻辑：
  // - 若 layer->projectorMgr 为空，分配 0x24 字节并调用
  //   Shadow_ProjectorManager_Init(mgr, layer)
  // - 返回 layer->projectorMgr
  return layer->projectorMgr;
}

// 0x6F713CA0
int Shadow_ProjectorManager_Add(ShadowProjectorManager* mgr, float x, float y,
                                float z, float sizeX, float sizeY,
                                float sizeZ, int callback, int arg0) {
  (void)mgr;
  (void)x;
  (void)y;
  (void)z;
  (void)sizeX;
  (void)sizeY;
  (void)sizeZ;
  (void)callback;
  (void)arg0;
  // 原版逻辑：
  // - 优先从 freeIndices 取条目，否则扩容 projectors 数组并 new 条目
  // - 调用 Shadow_Projector_Init 初始化条目
  // - 若初始化失败则回收并返回 -1
  return 0;
}

// 0x6F7290B0
int Shadow_Projector_Init(ShadowProjector* entry, void* tex, int callback,
                          float minX, float minY, float maxX, float maxY,
                          float strength) {
  if (!entry)
    return 0;

  // 释放旧的 ListB 资源
  Shadow_Projector_ReleaseListB(entry);

  entry->flags04 = 0;
  entry->flagA = (std::fabs(minX) < 0.00000023841858f) ? 1 : 0;
  entry->flagB = (std::fabs(minY) < 0.00000023841858f) ? 1 : 0;

  entry->ownerLayer = tex;
  entry->rangeX = static_cast<int32_t>(maxX) - static_cast<int32_t>(minX);
  entry->rangeY = static_cast<int32_t>(maxY) - static_cast<int32_t>(minY);
  entry->offsetX = static_cast<int32_t>(minX);
  entry->offsetY = static_cast<int32_t>(minY);
  entry->boundW = static_cast<int32_t>(maxX) - static_cast<int32_t>(minX);
  entry->boundH = static_cast<int32_t>(maxY) - static_cast<int32_t>(minY);
  entry->centerX = static_cast<int32_t>(minX);
  entry->centerY = static_cast<int32_t>(minY);

  float sizeXY[2] = {strength * 2.0f, strength * 2.0f};
  float center[2] = {strength, strength};
  int listBIndex = Shadow_ListB_Alloc(reinterpret_cast<TerrainShadowLayer*>(tex),
                                      callback, sizeXY, center, minX, 4);
  entry->listBIndex = listBIndex;
  if (listBIndex == -1)
    return 0;

  entry->gridMinX = static_cast<uint32_t>(minX * 256.0f);
  entry->gridMinY = static_cast<uint32_t>(minY * 256.0f);
  entry->gridMaxX = static_cast<uint32_t>(maxX * 256.0f);
  entry->gridMaxY = static_cast<uint32_t>(maxY * 256.0f);
  entry->gridSpan = entry->gridMinX + entry->gridMaxX + entry->gridMaxY;
  return 1;
}

// 0x6F732BA0
void Shadow_Projector_ReleaseListB(ShadowProjector* entry) {
  (void)entry;
}

// 0x6F713250
int Shadow_ListB_Alloc(TerrainShadowLayer* layer, int entryId,
                       const void* minXY, const void* maxXY,
                       int gridIdx, int flags) {
  (void)layer;
  (void)entryId;
  (void)minXY;
  (void)maxXY;
  (void)gridIdx;
  (void)flags;
  // 原版逻辑：
  // - 在 listB_ptr (0x2CC) 中遍历寻找 flags&1==0 的空槽
  // - 不足则扩容 listB_capacity，并初始化新条目
  // - 将坐标/纹理信息写入 ListBEntry
  return -1;
}

// 0x6F736750
void Shadow_ListB_Release(TerrainShadowLayer* layer, int index) {
  (void)layer;
  (void)index;
  // 原版逻辑：
  // - 清理 ListBEntry 中的纹理/资源指针
  // - 清除 flags 的 bit0
}

// 0x6F76D790
int Shadow_AddProjectorSimple(float x, float y, float z, float sizeX,
                              float sizeY, float sizeZ) {
  (void)x;
  (void)y;
  (void)z;
  (void)sizeX;
  (void)sizeY;
  (void)sizeZ;
  return 0;
}

// 0x6F76D800
int Shadow_AddProjectorFromObject(void* obj, int flags, float strength) {
  if (!obj)
    return -1;

  auto* templateEntry = reinterpret_cast<uint32_t*>(sub_6F76A490(flags));
  if (!templateEntry && !sub_6F763420())
    return -1;

  int texHandle = 0;
  int fallbackHandle = 0;
  if (!flags)
    texHandle = templateEntry[9];
  if (!strength)
    fallbackHandle = templateEntry[8];

  int callback = sub_6F050D00(templateEntry + 21);
  int texState = templateEntry[10];

  int arg0 = *reinterpret_cast<int*>(obj);
  int arg1 = *reinterpret_cast<int*>(obj);
  int arg2 = *reinterpret_cast<int*>(obj);
  int arg3 = templateEntry[11];

  int terrain = GetTerrain_771060();
  sub_6F7276D0(terrain);
  return sub_6F713CA0(reinterpret_cast<int>(obj), arg3, arg1, arg2, arg0,
                      fallbackHandle, texHandle, texState, callback);
}

} // namespace native
} // namespace war3
