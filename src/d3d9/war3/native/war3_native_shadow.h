// war3_native_shadow.h
// 地形阴影系统逆向还原（Game.dll 1.27.x）
//
// 说明：
// - 该文件用于“还原原版阴影链路”与结构语义标注。
// - 仍有大量字段未知，已根据 IDA 汇编做最小可信映射。
// - 任何不确定字段均用 unknown/pad 命名并标注来源偏移。

#pragma once

#include <cstdint>

namespace war3 {
namespace native {

// ============================================================================
// 影层基础结构（Terrain / ShadowLayer）
// ============================================================================

// 前置声明
struct ShadowProjector;
struct ShadowProjectorManager;
struct ShadowListBEntry;
struct TerrainShadowGrid;

// 影层类型表（sub_6F725F70 使用：entryId * 0x4C + tableBase）
struct ShadowTypeInfo {
  uint8_t pad_0000[0x10]; // +0x00
  void* stateBlock0;      // +0x10 (用于 sub_6F0E35B0 的参数)
  void* stateBlock1;      // +0x14
  uint8_t pad_0018[0x0C]; // +0x18
  void* vbOrMesh0;        // +0x24 (sub_6F0E35B0 参数)
  uint8_t pad_0028[0x0C]; // +0x28
  void* vbOrMesh1;        // +0x34 (sub_6F0E35B0 参数)
  uint8_t pad_0038[0x08]; // +0x38
  uint32_t texA;          // +0x40 (sub_6F705090 参数)
  uint32_t texB;          // +0x44
  uint8_t pad_0048[0x04]; // +0x48
}; // size 0x4C

// 阴影投影器池（sub_6F70CBA0 / sub_6F713CA0）
struct ShadowProjectorManager {
  void* ownerLayer;          // +0x00 (TerrainShadowLayer*)
  uint32_t projectorCap;     // +0x04
  uint32_t projectorCount;   // +0x08
  ShadowProjector** projectors; // +0x0C (ShadowProjector*[])
  uint32_t projectorGrow;    // +0x10
  uint32_t freeCap;          // +0x14
  uint32_t freeCount;        // +0x18
  uint32_t* freeIndices;     // +0x1C
  uint32_t freeGrow;         // +0x20
}; // size 0x24

// 单个投影器条目（sub_6F7290B0 初始化，sub_6F732BA0 释放）
// 说明：字段语义尚未完全确定，以偏移为准。
struct ShadowProjector {
  void* ownerLayer;     // +0x00 (TerrainShadowLayer*)
  uint32_t flags04;     // +0x04
  uint32_t gridSpan;    // +0x08
  int32_t rangeX;       // +0x0C
  int32_t rangeY;       // +0x10
  int32_t offsetX;      // +0x14
  int32_t offsetY;      // +0x18
  int32_t boundW;       // +0x1C
  int32_t boundH;       // +0x20
  int32_t centerX;      // +0x24
  int32_t centerY;      // +0x28
  int32_t listBIndex;   // +0x2C (sub_6F713250 返回)
  uint32_t gridMinX;    // +0x30 (ftoui)
  uint32_t gridMinY;    // +0x34
  uint32_t gridMaxX;    // +0x38
  uint32_t gridMaxY;    // +0x3C
  uint32_t gridMaxSum;  // +0x40
  uint32_t gridMaxEnd;  // +0x44
  uint32_t flagA;       // +0x48 (bool)
  uint32_t flagB;       // +0x4C (bool)
}; // size 0x50

// 影层上下文（部分字段来自 0x6F737620 / 0x6F737400 / 0x6F737500）
struct TerrainShadowLayer {
  uint8_t pad_0000[0x0B0]; // +0x000
  ShadowProjectorManager* projectorMgr; // +0x0B0 (sub_6F7276D0)
  uint32_t gridWidth;      // +0x0B4 (sub_6F73DC00 使用)
  uint32_t gridHeight;     // +0x0B8
  uint8_t pad_00BC[0x54];  // +0x0BC
  uint32_t listA_count;    // +0x110
  void** listA_ptrs;       // +0x114 (ShadowListAEntry*[])
  uint8_t pad_0118[0x34];  // +0x118
  void* shadowTypeTable;   // +0x14C (ShadowTypeInfo[] base)
  uint8_t pad_0150[0x174]; // +0x150
  uint32_t listB_capacity; // +0x2C4
  uint32_t listB_count;    // +0x2C8
  ShadowListBEntry* listB_ptr; // +0x2CC
  uint32_t listB_growStep; // +0x2D0
  uint8_t pad_02D4[0x688]; // +0x2D4
  uint32_t unknown_95C;    // +0x95C (用于 sub_6F7377B0)
  uint8_t pad_0960[0x10];  // +0x960
  uint32_t flags_970;      // +0x970 (bit0/bit6/bit7/bit8/bit10/bit15)
  uint8_t pad_0974[0xE8];  // +0x974
};

// ============================================================================
// List A: 地形阴影纹理/雾/边界列表
// ============================================================================

// ListA 子节点（渲染时遍历：RenderShadowListA 内部）
struct ShadowListAGroupNode {
  void* vtable_or_pad;     // +0x00
  ShadowListAGroupNode* next; // +0x04
  uint8_t pad_0008[0x38];  // +0x08
  void* stateBlockA;       // +0x40
  void* stateBlockB;       // +0x44
  uint8_t pad_0048[0x0C];  // +0x48
  void* meshPtr;           // +0x54 (sub_6F0E35B0 参数)
  uint8_t pad_0058[0x0C];  // +0x58
  void* vbPtr;             // +0x64
  uint8_t pad_0068[0x08];  // +0x68
  uint32_t childCount;     // +0x70
  void* childList;         // +0x74 (ShadowListAChild*)
};

// ListA 子节点中的子条目（size = 0x18）
struct ShadowListAChild {
  uint8_t pad_0000[0x0C]; // +0x00
  uint32_t texHandle;     // +0x0C (sub_6F705090 参数)
  uint32_t texArg;        // +0x10
  uint8_t pad_0014[0x04]; // +0x14
};

// ListA 组头（RenderShadowListA 里按 0x24 stride 前进）
struct ShadowListAGroupHeader {
  ShadowListAGroupNode* head; // +0x00
  int32_t countOrFlag;        // +0x04 (推测)
  uint8_t pad_0008[0x1C];     // +0x08
};

// ListA 条目（size = 0x94, sub_6F73DC00 初始化）
struct ShadowListAEntry {
  uint32_t typeId;       // +0x00 (用于 shadowTypeTable 查表)
  uint32_t unk04;        // +0x04
  uint32_t groupCountA;  // +0x08 (sub_6F737860 使用)
  void* groupListA;      // +0x0C (ShadowListAGroupHeader[])
  uint8_t pad_0010[0x20];// +0x10
  int32_t gridX;         // +0x30 (sub_6F73D9F0: arg4)
  int32_t gridY;         // +0x34 (sub_6F73D9F0: arg8)
  float posX;            // +0x38 (sub_6F73D9F0 计算)
  float posY;            // +0x3C
  uint8_t pad_0040[0x0C];// +0x40
  uint16_t flagsWord;    // +0x4C
  uint8_t flagsByte;     // +0x4E
  uint8_t pad_004F[0x01];// +0x4F
  void* ownerWorld;      // +0x50 (sub_6F73D9F0: [edi+0x940])
  uint32_t unk54;        // +0x54
  uint32_t groupCountB;  // +0x58 (RenderShadowListA 使用)
  void* groupListB;      // +0x5C (ShadowListAGroupHeader[])
  uint32_t unk60;        // +0x60
  void* cachedResource;  // +0x64 (RenderShadowListA sub_6F139620)
  uint32_t unk68;        // +0x68
  void* unk6C;           // +0x6C
  uint8_t pad_0070[0x08];// +0x70
  void* layerTex;        // +0x78 (RenderShadowListA 纹理)
  void* layerTex2;       // +0x7C
  void* blobPtr;         // +0x80 (RenderShadowListA: !=0 才会绘制)
  uint32_t blobArg;      // +0x84 (sub_6F705090 参数)
  uint32_t unk88;        // +0x88
  void* ownerTerrain;    // +0x90 (sub_6F73D9F0: edi)
};

// ============================================================================
// List B: 单位/动态阴影列表（size = 0xA0）
// ============================================================================

struct ShadowListBEntry {
  uint32_t id;           // +0x00
  uint32_t flags;        // +0x04 (bit0=占用; bit2/bit5/bit6/bit9)
  uint8_t pad_0008[0x4C];// +0x08
  uint32_t slotA;        // +0x54
  uint32_t slotB;        // +0x58
  uint8_t pad_005C[0x08];// +0x5C
  uint32_t vbCountA;     // +0x64
  uint32_t vbCountB;     // +0x68
  uint32_t vbCountC;     // +0x6C
  uint8_t pad_0070[0x08];// +0x70
  uint32_t texA;         // +0x78
  uint8_t pad_007C[0x08];// +0x7C
  uint32_t slotCompare;  // +0x84 (与 slotA 比较)
  void* altBlock;        // +0x88
  uint32_t texB;         // +0x94
  uint32_t texC;         // +0x98
  uint8_t pad_009C[0x04];// +0x9C
}; // size 0xA0

// ============================================================================
// 阴影贴图更新（建筑/地形投影写入路径）
// ============================================================================

// 影子更新项（size = 0x78）- sub_6F73FA20 遍历
struct ShadowUpdateEntry {
  uint32_t unk00;        // +0x00
  uint32_t active;       // +0x04 (0=跳过)
  uint32_t flags08;      // +0x08
  uint32_t flags0C;      // +0x0C (为0时可触发自动移除)
  int32_t boundMinX;     // +0x10
  int32_t boundMinY;     // +0x14
  int32_t boundMaxX;     // +0x18
  int32_t boundMaxY;     // +0x1C
  uint32_t useRadius;    // +0x20 (非0时进行半径检查)
  float centerX;         // +0x24
  float centerY;         // +0x28
  float radiusSq;        // +0x2C
  void* callback;        // +0x30 (函数指针，写入阴影强度)
  uint32_t lifeMax;      // +0x34
  uint32_t lifeNow;      // +0x38
  uint32_t interval;     // +0x3C
  uint32_t lastTick;     // +0x40
  uint32_t intervalTick; // +0x44
  uint32_t lastWrite;    // +0x48
  uint32_t flags4C;      // +0x4C
  uint32_t cbArgA;       // +0x50
  uint32_t cbArgB;       // +0x54
  uint32_t cbArgC;       // +0x58
  uint32_t fadeEnabled;  // +0x5C
  uint32_t fadeDuration; // +0x60
  uint32_t fadeStart;    // +0x64
  uint8_t pad_0068[0x08];// +0x68
  float* gridValues;     // +0x70 (写入目标)
}; // size 0x78

// 阴影更新列表（owner+0x790）
struct ShadowUpdateList {
  uint32_t capacity;     // +0x00
  uint32_t count;        // +0x04
  ShadowUpdateEntry* list; // +0x08
  uint32_t unk0C;        // +0x0C
  uint32_t timeAccum;    // +0x10
  uint32_t unk14;        // +0x14
  TerrainShadowGrid* ownerTerrain; // +0x18 (Shadow_UpdateEntry_Write 使用 +0xB4/+0xE4)
  uint32_t unk1C;        // +0x1C
  uint8_t pad_0020[0x100];// +0x20
}; // size >= 0x120

// 地形网格（用于 Shadow_UpdateEntry_Write）
struct TerrainShadowGrid {
  uint8_t pad_0000[0x0B4]; // +0x000
  uint32_t gridWidth;      // +0x0B4
  uint8_t pad_00B8[0x2C];  // +0x0B8
  uint32_t* packedGrid;    // +0x0E4
};

// ============================================================================
// 阴影链路函数（命名/语义还原）
// ============================================================================

// 0x6F73DE20: 判断阴影层是否启用（flags_970）
int Terrain_Shadow_IsLayerEnabled(TerrainShadowLayer* layer);

// 0x6F73DE40: 读取阴影模式标志（flags_970 bit6）
int Terrain_Shadow_GetAltMode(TerrainShadowLayer* layer);

// 0x6F737620: 阴影层入口（按 a2/a3 控制 A/B 列表）
void Terrain_Shadow_RenderLayer(TerrainShadowLayer* layer, int a2, int a3,
                                int a4);

// 0x6F737500: ListA 单条绘制
int Terrain_Shadow_RenderListA(TerrainShadowLayer* layer,
                               ShadowListAEntry* entry);

// 0x6F737400: ListB 批量绘制
void Terrain_Shadow_RenderListB(TerrainShadowLayer* layer, int idFilter,
                                int passType);

// 0x6F737310: ListB 单条绘制
int Terrain_Shadow_RenderListBEntry(TerrainShadowLayer* layer,
                                    ShadowListBEntry* entry);

// 0x6F7377B0: ListA 组节点绘制
int Terrain_Shadow_RenderGroupEntry(TerrainShadowLayer* layer,
                                    ShadowListAGroupHeader* groupHeader,
                                    int stateId, int useAltState);

// 0x6F737860: ListA 组列表绘制
void Terrain_Shadow_RenderGroups(TerrainShadowLayer* layer,
                                 ShadowListAEntry* entry);

// 0x6F7376E0: ListA 前置整理/刷新
void Terrain_Shadow_PrepareListA(TerrainShadowLayer* layer);

// 0x6F73DC00: 初始化 ShadowListAEntry 网格
void Terrain_Shadow_InitGrid(TerrainShadowLayer* layer);

// 0x6F73D9F0: 初始化 ShadowListAEntry
void Terrain_Shadow_InitEntry(TerrainShadowLayer* layer,
                              ShadowListAEntry* entry, int gridX, int gridY);

// 0x6F73DF20: 标记 ListA 条目需要更新
void Terrain_Shadow_MarkEntryDirty(void* shadowUpdateList, int index,
                                   int payload);

// 0x6F73FA20: 阴影更新列表执行（写入贴图）
void Shadow_UpdateList_Run(ShadowUpdateList* list, float deltaTime);

// 0x6F73F7A0: 阴影贴图写入（核心写入点）
int Shadow_UpdateEntry_Write(ShadowUpdateList* list, ShadowUpdateEntry* entry,
                             float strength);

// 0x6F70CBA0: 初始化投影器池
ShadowProjectorManager* Shadow_ProjectorManager_Init(
    ShadowProjectorManager* mgr, TerrainShadowLayer* owner);

// 0x6F70CB70: 初始化投影器条目（清理旧资源）
ShadowProjector* Shadow_Projector_Reset(ShadowProjector* entry);

// 0x6F7276D0: 获取/创建投影器池（TerrainShadowLayer+0xB0）
ShadowProjectorManager* Shadow_ProjectorManager_GetOrCreate(
    TerrainShadowLayer* layer);

// 0x6F713CA0: 投影器加入列表（ProjectorManager）
int Shadow_ProjectorManager_Add(ShadowProjectorManager* mgr, float x, float y,
                                float z, float sizeX, float sizeY, float sizeZ,
                                int callback, int arg0);

// 0x6F7290B0: 初始化投影器条目
int Shadow_Projector_Init(ShadowProjector* entry, void* tex, int callback,
                          float minX, float minY, float maxX, float maxY,
                          float strength);

// 0x6F732BA0: 释放投影器占用的 ListB 资源
void Shadow_Projector_ReleaseListB(ShadowProjector* entry);

// 0x6F713250: 从 ListB 池分配条目（返回索引，-1 为失败）
int Shadow_ListB_Alloc(TerrainShadowLayer* layer, int entryId,
                       const void* minXY, const void* maxXY,
                       int gridIdx, int flags);

// 0x6F736750: 释放 ListB 条目
void Shadow_ListB_Release(TerrainShadowLayer* layer, int index);

// 0x6F76D790 / 0x6F76D800: 外部入口（地形/建筑调用）
int Shadow_AddProjectorSimple(float x, float y, float z, float sizeX,
                              float sizeY, float sizeZ);
int Shadow_AddProjectorFromObject(void* obj, int flags, float strength);

} // namespace native
} // namespace war3
