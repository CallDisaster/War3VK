// war3_native_renderer.h
// War3 Native Renderer - 完整还原魔兽争霸3渲染链
// 基于 IDA Pro 逆向分析 (Game.dll 1.27.x)
//
// 本文件提供了完整的War3渲染系统数据结构和函数原型，
// 可以直接替换原版游戏函数使用。

#pragma once

#include <cstdint>

// 地形阴影链路（完整逆向）
#include "war3_native_shadow.h"

#ifdef _MSC_VER
#define WAR3_NATIVE_CB __fastcall
#define WAR3_NATIVE_STDCALL __stdcall
#else
#define WAR3_NATIVE_CB __attribute__((fastcall))
#define WAR3_NATIVE_STDCALL __attribute__((stdcall))
#endif

namespace war3 {
namespace native {

// ============================================================================
// 前置声明
// ============================================================================
struct CWorldFrameWar3;
struct SceneNode;
struct SceneNodeChildBucket;
struct RenderBatchElement;
struct ListHeader;  
struct CGameUI;
struct CCamera;
struct CCameraWar3;
struct CStringRep;
struct CUnit;
struct CSprite;
struct CSpriteUber;
struct CModel;
struct CModelData;
struct CGeoset;
struct CGeosetData;
struct CModelAnimController;
struct CModelInstance;
struct CEffect;
struct CEffectFloating;
struct CMiniMap;
struct CInfoBar;
struct CCommandBar;
struct CResourceBar;
struct CUpperButtonBar;
struct CHeroBar;
struct CPeonBar;
struct CPortraitButton;
struct CTimeOfDayIndicator;
struct CChatEditBar;
struct CCinematicPanel;
struct CLight;
struct CGxuLight;

// ============================================================================
// 渲染阶段定义
// ============================================================================

enum class RenderStage : int32_t {
  Stage0_PreRenderContext = 0,                // sub_6F186300(this+0x354)
  Stage1_TerrainShadow0 = 1,                  // TerrainShadow_Dispatch(0)
  Stage2_TerrainShadow1 = 2,                  // TerrainShadow_Dispatch(1)
  Stage3_TerrainShadow2 = 3,                  // TerrainShadow_Dispatch(2)
  Stage4_TerrainShadow3 = 4,                  // TerrainShadow_Dispatch(3)
  Stage5_TerrainShadow5 = 5,                  // TerrainShadow_Dispatch(5)
  Stage6_TerrainShadow8 = 6,                  // TerrainShadow_Dispatch(8)
  Stage7_TerrainShadow9 = 7,                  // TerrainShadow_Dispatch(9)
  Stage8_TerrainShadow10 = 8,                 // TerrainShadow_Dispatch(10)
  Stage9_TerrainShadow6 = 9,                  // TerrainShadow_Dispatch(6)
  Stage10_TerrainShadow4 = 10,                // TerrainShadow_Dispatch(4)
  Stage11_TerrainShadow12_Group0 = 11,        // TerrainShadow_Dispatch(12)+group0
  Stage12_Group1 = 12,                        // WorldObjects_RenderGroup(group1)
  Stage13_Group2 = 13,                        // WorldObjects_RenderGroup(group2)
  Stage14_TerrainShadow7 = 14,                // TerrainShadow_Dispatch(7)
  Stage15_SelectionManager = 15,              // selection manager stage
  Stage16_DebugOverlay = 16,                  // 调试/覆盖层 bitmask 链
  Stage17_TerrainShadow11 = 17,               // TerrainShadow_Dispatch(11)
  Stage18_PostProcessTail = 18,               // 预览/后处理尾链
  Stage19_TerrainShadow14 = 19,               // TerrainShadow_Dispatch(14)
  Stage20_TerrainShadow15 = 20,               // TerrainShadow_Dispatch(15)
  Stage21_TerrainShadow13_IndicatorTail = 21, // TerrainShadow_Dispatch(13)+尾链

  // 旧命名兼容别名。保留仅为了避免后续实验代码编译直接断裂，
  // 但后续继续还原时应优先使用上面的“阶段号+真实作用”命名。
  SkyBox = Stage0_PreRenderContext,
  Terrain = Stage1_TerrainShadow0,
  Buildings_Legacy = Stage2_TerrainShadow1,
  Decorations_Legacy = Stage3_TerrainShadow2,
  OtherObjects_Legacy = Stage4_TerrainShadow3,
  TransparentDecor_Legacy = Stage5_TerrainShadow5,
  Other1_Legacy = Stage6_TerrainShadow8,
  Other2_Legacy = Stage7_TerrainShadow9,
  Effects_Legacy = Stage8_TerrainShadow10,
  Units_Legacy = Stage9_TerrainShadow6,
  TransparentEffects_Legacy = Stage10_TerrainShadow4,
  TransparentOther_Legacy = Stage11_TerrainShadow12_Group0,
  ShadowGroup_Legacy = Stage12_Group1,
  TerrainDetail_Legacy = Stage13_Group2,
  TransparentBuildings_Legacy = Stage14_TerrainShadow7,
  EditorSpecial_Legacy = Stage15_SelectionManager,
  Particles_Legacy = Stage17_TerrainShadow11,
  WaterFog_Legacy = Stage19_TerrainShadow14,
  PostProcess_Legacy = Stage18_PostProcessTail,
  UI_Legacy = Stage21_TerrainShadow13_IndicatorTail,
};

// categoryMode：模式状态机值域（对应IDA a3参数）
enum class CategoryMode : int32_t {
  Invalid = -1,  // 无效值
  Default = 0,   // stage0 前置上下文
  Standard = 1,  // 主世界不透明/基础地形路径
  Overlay = 2,   // 透明/覆盖/指示器相关路径
  Mode3 = 3,     // activeQueue 非空时强制切换到此模式
};

// renderCategory：位掩码风格（对应IDA a4参数，与world[409]比较）
enum class RenderCategoryMask : int32_t {
  Invalid = -1,  // 无效值
  Stage0 = 1,    // 仅 stage0 预渲染上下文出现
  Opaque = 2,    // 主世界地形/对象路径
  Overlay = 4,   // 透明/尾链/指示器路径
  Shadow = 8,    // 旧研究中的候选值，本专题主链未直接命中
};

enum class WorldGroupIndex : int32_t {
  Group0 = 0, // world[91] / this+0x16C
  Group1 = 1, // world[92] / this+0x170
  Group2 = 2, // world[93] / this+0x174

  Units_Legacy = Group0,
  Buildings_Legacy = Group1,
  Effects_Legacy = Group2,
  LegacyGroup3_Unverified = 3,
};

class IRenderStateObserver {
public:
  virtual void Destroy() = 0;           // vtable[0]
  virtual void Unk1() = 0;              // vtable[1]
  virtual void Unk2() = 0;              // vtable[2]
  virtual void OnCategoryEnable() = 0;  // vtable[3] (猜测)
  virtual void OnCategoryDisable() = 0; // vtable[4] (确认为 +16)
};

// ============================================================================
// 数据结构定义
// ============================================================================

struct ListHeader {
  uint32_t reserved0[3]; // +0x00 ~ +0x08: 未知
  void *data;            // +0x0C: 条目数组指针
  uint32_t reserved10;   // +0x10: 未知
  uint32_t count;        // +0x14: 元素数量
}; // sizeof = 24 (0x18) bytes

template <typename T>
struct War3CompactArrayHeader {
  uint32_t capacity; // +0x00
  uint32_t count;    // +0x04
  T *data;           // +0x08
}; // sizeof = 0x0C

template <typename T>
struct War3GrowableArrayHeader {
  uint32_t capacity;         // +0x00
  uint32_t count;            // +0x04
  T *data;                   // +0x08
  uint32_t growth_alignment; // +0x0C
}; // sizeof = 0x10

// 世界对象列表条目
struct WorldObjectListEntry {
  void *objectEntry;   // +0x00: WorldObjectEntry指针
  uint32_t unknown[5]; // +0x04 ~ +0x18: 未知字段
}; // 总大小: 24 bytes

// 世界对象入口
struct WorldObjectEntry {
  void *vtable;         // +0x00: 虚函数表
  uint8_t padding1[32]; // +0x04 ~ +0x1F: 未知
  void *sceneNode;      // +0x20: SceneNode指针
                        // ...

  // vtable偏移:
  // [0]: 析构函数
  // [1-4]: 未知
  // [5] (+0x14): void __thiscall PreRender(WorldObjectEntry* this)
  //     - 渲染前准备函数
}; // 最小大小: 36+ bytes

struct CFog {
  // 基础对象头
  void *vtable;

  // +0x04: 雾气开关/样式
  // 0 = None, 1 = Linear, 2 = Exp, 3 = Exp2
  uint32_t style;

  // +0x08 ~ 0x14: 颜色数据 (这是它随时间变化的地方！)
  // 可能是 byte[4] 也可能是 float[4] (R, G, B, A)
  // 暴雪习惯用 float 做计算，渲染时转 byte
  float colorR; // +0x08
  float colorG; // +0x0C
  float colorB; // +0x10
  float colorA; // +0x14

  // +0x18: 密度 (对于 Exp/Exp2 模式)
  float density;

  // +0x1C: 起始距离 (Z-Start) - 雾从多远开始出现
  float startDepth;

  // +0x20: 结束距离 (Z-End) - 雾在多远彻底看不见
  float endDepth;

  // ... 后面可能还有"目标颜色"(TargetColor) 用于插值
};

struct CRenderListNode {
    CRenderListNode* pPrev;      // 0x00
    CRenderListNode* pNext;      // 0x04 (i = *(_DWORD *)(i + 4))

    // 这里的 Data 指向 CGameEntity 或 CFog 等对象
    // sub_6F1914D0 会调用它的虚函数 vtable[4]
    void* pData;                 // 0x08 (*(void **)(i + 8))

    uint32_t nCategoryFlag;      // 0x0C (i + 12)
};

struct SceneNode;
struct SceneNodeTintRecord;

struct SceneNodeChildLink {
  int32_t             ukn_0x0;      // 0x00
  SceneNodeChildLink* next_link;    // 0x04
  SceneNode*          child_scene;  // 0x08 这套 link 也会被动画/stage-preset 递归复用
  uint32_t            link_flags;   // 0x0C bit0=允许递归传播到 child_scene
}; // sizeof = 0x10

struct SceneNodeChildBucket {
  uint32_t            ukn_0x0;      // 0x00
  uint32_t            ukn_0x4;      // 0x04
  SceneNodeChildLink* first_link;   // 0x08
}; // sizeof = 0x0C

// 场景节点 (1.27.x)
struct SceneNode {
  void *vtable;             // +0x00: 虚函数表
  uint8_t padding1[8];      // +0x04 ~ +0x0B
  uint32_t renderableCount; // +0x0C: 可渲染对象数量
  void **renderableList;    // +0x10: 可渲染对象列表
  uint8_t padding2[12];     // +0x14 ~ +0x1F
  SceneNodeTintRecord *cullTable; // +0x20: 16 字节颜色/剔除记录表
  uint8_t padding3[12];     // +0x24 ~ +0x2F
  void **meshInfoTable;     // +0x30: MeshInfo 表 (指针数组)
  uint8_t padding4[28];     // +0x34 ~ +0x4F
  uint8_t *visibilityTable; // +0x50: layer 可见性/alpha 表
  uint8_t padding5[16];     // +0x54 ~ +0x63
  float worldMatrix[12];    // +0x64: 世界变换矩阵 (3x4)
  uint32_t flags;           // +0x94: 标志位 (bit4=存在透明附加列表)
  void *childVisibilityContext; // +0x98: 子节点可见性查询上下文
  void *world;              // +0x9C: World/CWorldFrameWar3 关联
  uint32_t stagePresetBaseIndex; // +0xA0: 共享 stage preset 池起始 index
  uint8_t padding7[32];     // +0xA4 ~ +0xC3
  uint32_t childCount;      // +0xC4: 子节点数量
  SceneNodeChildBucket* childPtrArray; // +0xC8: 子节点 bucket 数组（stride 0x0C）
  void *childVisFlags;      // +0xD4: 子节点可见性缓存字节表；动画/stage-preset 递归也会复用
  // ...
}; // 最小大小: 0x158 bytes (344 bytes)

// CWorldFrameWar3 - 游戏世界渲染上下文 (1.27.x)
// 本专题已确认对象大小为 0x668（由 CGameUI ctor 分配 1640 字节得到）。
struct CWorldFrameWar3 {
    void *vtable;                      // 0x00
    uint32_t refCount;                 // 0x04
    uint8_t pad_0x08[0x144];           // 0x08
    CRenderListNode *m_RenderCategoryList; // 0x14C：类别观察者链表
    uint8_t pad_0x150[0x1C];           // 0x150
    ListHeader *worldGroup0;           // 0x16C：WorldObjects group0
    ListHeader *worldGroup1;           // 0x170：WorldObjects group1
    ListHeader *worldGroup2;           // 0x174：WorldObjects group2
    uint8_t pad_0x178[0x24];           // 0x178
    CCameraWar3 *camera;               // 0x19C
    uint32_t reserved1A0;              // 0x1A0
    uint32_t clickRouteMaskA;          // 0x1A4：点击/跟踪事件主掩码
    uint32_t clickRouteMaskB;          // 0x1A8：点击/跟踪事件副掩码
    uint32_t playerColorMode;          // 0x1AC：玩家颜色/跟踪颜色模式
    uint32_t playerColorStackCapacity; // 0x1B0
    uint32_t playerColorStackSize;     // 0x1B4
    uint32_t *playerColorStack;        // 0x1B8
    uint32_t playerColorStackGrowStep; // 0x1BC
    uint32_t suppressTrackPop;         // 0x1C0：PopTrackState 时的抑制标记
    uint32_t worldFrameVisible;        // 0x1C4：OnShow/OnHide 切换
    uint32_t playerColorLabelRaw[3];   // 0x1C8：RCString 原始布局
    float min_y;                       // 0x1D4
    float min_x;                       // 0x1D8
    float max_y;                       // 0x1DC
    float max_x;                       // 0x1E0
    uint8_t pad_0x1E4[0x5C];           // 0x1E4
    void *trackedSpriteHandle;         // 0x240：当前跟踪中的 sprite/对象句柄
    void *trackedSpriteRef;            // 0x244：跟踪对象附带引用
    uint32_t stage18PreviewEnabled;    // 0x248：stage18 前半段门控
    uint8_t pad_0x24C[0x04];           // 0x24C
    void *stage18PreviewContext;       // 0x250：stage18 预览上下文
    uint8_t renderSceneCleanupContext[0xA4]; // 0x254：sub_6F3ACCF0 直接吃 this+0x254
    uint32_t stage21StateFlags;        // 0x2F8：stage21 尾链前置状态
    uint32_t stage21IndicatorReady;    // 0x2FC：stage21 是否可提交图像
    int32_t shadowModeOrStage21ListBEntryIndex; // 0x300：RenderScene 前半段作 shadow mode 参数，stage21 尾链作 ListB entry index
    uint8_t pad_0x304[0x0C];           // 0x304
    float mouse_x;                     // 0x310
    float mouse_y;                     // 0x314
    float mouse_z;                     // 0x318
    void *activeRenderQueue;           // 0x31C
    uint32_t inputSuppressed;          // 0x320
    uint32_t stage17Enabled;           // 0x324
    uint8_t pad_0x328[0x0C];           // 0x328
    void *terrainFogRuntime;           // 0x334：构造阶段创建的 terrain/fog 运行时对象
    void *dayNightTerrainRuntime;      // 0x338：每帧 StateCleanup 的运行时对象 A
    void *dayNightUnitRuntime;         // 0x33C：每帧 StateCleanup 的运行时对象 B
    void *extraEnvironmentRuntime;     // 0x340：构造阶段创建的附加环境对象
    uint8_t pad_0x344[0x04];           // 0x344
    uint32_t reservedRcString348[3];   // 0x348：RCString 原始布局（用途待继续命名）
    void *stage0Context;               // 0x354：RenderScene stage0 / RallyIndicatorSrc 上下文
    uint32_t stage0GateEnabled;        // 0x358
    uint32_t stage0GateReady;          // 0x35C
    uint8_t pad_0x360[0x28];           // 0x360
    uint32_t scaledAnimTimeEnabled;    // 0x388：ctor 默认 1
    uint32_t reserved38C;              // 0x38C
    float scaledAnimTimeAccum;         // 0x390
    float dayHoursSeed;                // 0x394：ctor 置为 +INF
    uint32_t miscScaledAnimTimeKey;    // 0x398："ScaledAnimTime"
    uint32_t miscDayHoursKey;          // 0x39C："DayHours"
    uint32_t colorFriendKey;           // 0x3A0："ColorFriend"
    uint32_t colorNeutralKey;          // 0x3A4："ColorNeutral"
    uint32_t colorEnemyKey;            // 0x3A8："ColorEnemy"
    uint8_t pad_0x3AC[0x190];          // 0x3AC
    uint32_t rallyIndicatorDstCapacity; // 0x53C
    uint32_t rallyIndicatorDstCount;   // 0x540
    void *rallyIndicatorDstArray;      // 0x544：16 * 0x18 条目
    uint8_t pad_0x548[0x40];           // 0x548
    void *rallyIndicatorSrcObject;     // 0x588："RallyIndicatorSrc"
    void *targetPointConfirmObject;    // 0x58C："TargetPointConfirm"
    uint32_t waypointIndicatorCapacity; // 0x590
    uint32_t waypointIndicatorCount;   // 0x594
    void *waypointIndicatorArray;      // 0x598：256 * 0x1C 条目
    uint8_t pad_0x59C[0x08];           // 0x59C
    uint32_t deferredCallbackCountA;   // 0x5A4
    void **deferredCallbackArrayA;     // 0x5A8
    uint32_t reserved5AC;              // 0x5AC
    uint32_t reserved5B0;              // 0x5B0
    uint32_t deferredCallbackCountB;   // 0x5B4
    void **deferredCallbackArrayB;     // 0x5B8
    uint32_t reserved5BC;              // 0x5BC
    uint32_t reserved5C0;              // 0x5C0
    uint32_t deferredCallbackCountC;   // 0x5C4
    void **deferredCallbackArrayC;     // 0x5C8
    uint32_t reserved5CC;              // 0x5CC
    uint32_t reserved5D0;              // 0x5D0
    uint32_t deferredCallbackCountD;   // 0x5D4
    void **deferredCallbackArrayD;     // 0x5D8
    uint32_t reserved5DC;              // 0x5DC
    uint32_t reserved5E0;              // 0x5E0
    uint32_t deferredCallbackCountE;   // 0x5E4
    void **deferredCallbackArrayE;     // 0x5E8
    uint32_t reserved5EC;              // 0x5EC
    uint32_t debugOverlayTogglePrimary; // 0x5F0
    uint32_t deferredCallbackCountF;   // 0x5F4
    void **deferredCallbackArrayF;     // 0x5F8
    uint32_t reserved5FC;              // 0x5FC
    uint32_t debugOverlayToggleSecondary; // 0x600
    uint8_t pad_0x604[0x40];           // 0x604
    uint32_t deferredSelectionCount;   // 0x644：sub_6F368E00
    void **deferredSelectionArray;     // 0x648：sub_6F368E00
    uint8_t pad_0x64C[0x14];           // 0x64C
    CategoryMode currentCategoryMode;  // 0x660
    RenderCategoryMask currentRenderCategory; // 0x664

    // ------------------------------------------------------------------------
    // 成员函数：还原 RenderCategory_Disable
    // ------------------------------------------------------------------------
    int RenderCategory_Disable(int categoryMask, int expectedValue) {
        int result = 0;
        
        // 遍历 0x14C 处的链表
        for (CRenderListNode* node = this->m_RenderCategoryList; 
             node != nullptr; 
             node = node->pNext) 
        {
            // result = a2 & *(_DWORD *)(i + 12);
            result = categoryMask & node->nCategoryFlag;
            
            if (result == expectedValue) {
                // result = sub_6F1914D0(*(void **)(i + 8));
                // 这里的 Data 指向 CGameEntity，调用它的 vtable[4]
                if (node->pData) {
                    // 模拟 __thiscall 调用: (*vtable[4])(this)
                    // 注意：这里的 +16 是字节偏移，16/4 = 4，即第5个虚函数
                    using RenderFunc = int(__thiscall*)(void*);
                    void** vtable = *(void***)(node->pData);
                    RenderFunc func = (RenderFunc)vtable[4];
                    if (func) {
                        result = func(node->pData);
                    }
                }
            }
        }
        return result;
    }
};
static_assert(sizeof(ListHeader) == 0x18, "ListHeader 大小不匹配");
static_assert(sizeof(WorldObjectListEntry) == 0x18,
              "WorldObjectListEntry 大小不匹配");
static_assert(sizeof(CWorldFrameWar3) == 0x668,
              "CWorldFrameWar3 大小应为 0x668");

// ============================================================================
// GxDevice结构（图形设备接口）
// ============================================================================

struct GxDevice {
  void *vtable; // +0x00: 虚函数表
                // ...
                // 关键vtable偏移:
                // +0x54 (84): void RenderSceneFlush()      // 场景刷新
                // +0x6C (108): void SetVertexBuffer(offset)   // 设置顶点缓冲
                // +0x70 (112): void DrawPrimitive()            // 绘制图元
                // +0x74 (116): void StateCleanup74()          // 状态清理1
                // +0x78 (120): void StateCleanup78()          // 状态清理2
                // +0x98 (152): void ApplyStateBlock(ptr)     // 应用状态块
};

// ============================================================================
// RenderBatchElement结构（核心批次元素）
// ============================================================================

struct RenderBatchElement {
  void *batchEntry;      // +0x00: RenderablePart指针
  uint32_t flags;        // +0x04: 状态位
                         //   bit0 (0x01): meshData->meshFlag != 0
                         //   bit1 (0x02): 当前 layer 之后仍有可见层
                         //   (flags & 3) == 3 时会进入 Dispatch_Special
  uint32_t layerIndex;   // +0x08: 对应材质的Layer索引（用于排序）
  uint32_t layerCounter; // +0x0C: 该 mesh 当前已处理的可见层序号
  void *layerStatePtr;   // +0x10: 指向36字节 LayerStateRecord
}; // sizeof = 20 (0x14) bytes

// ============================================================================
// AUCTransparentEntry结构（透明队列元素）
// ============================================================================

struct AUCTransparentEntry {
  uint32_t type; // +0x00: 类型码 (0=粒子, 2=缎带, 3=特效, 4=附着物, 5=自定义)
  uint32_t sortKey; // +0x04: 透明排序键
  float distSq;     // +0x08: 到相机距离平方
  void *payload;    // +0x0C: 对象/回调指针
  uint32_t arg1;    // +0x10: 回调参数1（type=5）
  uint32_t arg2;    // +0x14: 回调参数2（type=5）
}; // sizeof = 24 (0x18) bytes

// ============================================================================
// RenderablePart结构（可渲染部件）
// ============================================================================

struct RenderablePart {
  uint8_t padding1[8];    // +0x00 ~ +0x07: 未知
  uint32_t stagePresetSpanBaseIndex; // +0x08: CModel_AssignVisiblePartStagePresetSpan 写入的共享 preset span 起始 index
  void *meshData;         // +0x0C: MeshData指针
  uint32_t skipFlag;      // +0x10: 跳过标志/禁用标志（非0则跳过）
  void *sceneNodeBackPtr; // +0x14: 回指SceneNode（由RenderBatch_Submit写入）
                          // ...
}; // 最小大小: 24+ bytes

// SceneNode +0x20 指向的 16 字节记录。当前只确认前 7 个字节参与颜色乘算。
struct SceneNodeTintRecord {
  uint32_t rgba_base;     // +0x00: base RGBA
  uint8_t mod_r;          // +0x04
  uint8_t mod_g;          // +0x05
  uint8_t mod_b;          // +0x06
  uint8_t ukn_0x07;       // +0x07
  uint8_t ukn_0x08[0x8];  // +0x08
}; // sizeof = 0x10

struct GxStagePresetRecord {
  // 当前已确认这 48 字节会参与 3x4 变换矩阵乘法、按轴缩放和点变换。
  // 但它最终仍通过 GxDevice_UpdateStage 下发，因此先保守命名为 stage preset。
  uint8_t raw[0x30];
}; // sizeof = 0x30

// override graph 求值时传入的一组输出缓冲/句柄。
// 目前仅把已经被 node_type=1/2/4/7 和辅助 builder 明确消费的字段固化下来。
struct RenderOverridePresetChannelDataRecord {
  float channel0Float3Data[3];  // +0x00
  float channel1Vec4Data[4];    // +0x0C
  float channel2Float3Data[3];  // +0x1C
}; // sizeof = 0x28

struct RenderOverridePresetChannelResolverRecord {
  uint8_t channel0Resolver[0x1C]; // +0x00
  uint8_t channel1Resolver[0x1C]; // +0x1C
  uint8_t channel2Resolver[0x1C]; // +0x38
}; // sizeof = 0x54

// 这张 0x50 的辅助缓存不是最终输出表，而是 B560/B620/FB40/FD10 之间共享的中间变换记录。
struct RenderOverrideTransformAuxRecord {
  float resolvedVec3_0[3];      // +0x00
  float resolvedVec4_1[4];      // +0x0C
  float resolvedVec3_2[3];      // +0x1C
  float backupVec3_0[3];        // +0x28
  float backupVec4_1[4];        // +0x34
  float backupVec3_2[3];        // +0x44
}; // sizeof = 0x50

struct RenderOverrideDependencyGateOutputRecord {
  uint8_t  colorRgb[3];         // +0x00: 由 sub_6F77B7A0 更新的 RGB
  uint8_t  enableOrAlpha;       // +0x03: 由 sub_6F77B710 更新的启用/alpha 字节
  uint32_t ukn_0x04;            // +0x04
  float    resolvedWeight;      // +0x08
  float    alphaScale;          // +0x0C
}; // sizeof = 0x10

struct RenderOverrideCompactScalarOutputEntry {
  int32_t value;                // +0x00: 紧凑标量输出，默认常见为 -1
}; // sizeof = 0x04

struct RenderOverrideGraphOutputBundle {
  uint8_t              ukn_0x00[0x08];
  GxStagePresetRecord* primaryPresetOutputs;        // +0x08: 主 48B preset 输出表
  uint8_t              ukn_0x0C[0x08];
  void*                gxuLightArrayHandle;         // +0x14: +8 -> CGxuLight*[]，node_type=1 使用
  void*                particleEmitterRuntimeArrayHandle; // +0x18: +8 -> CParticleEmitterRuntime[]，node_type=4 使用
  uint8_t              ukn_0x1C[0x08];
  GxStagePresetRecord* sharedPresetOutputs;         // +0x24: node_type=2 写入的共享 48B preset 输出
  uint8_t              ukn_0x28[0x04];
  RenderOverrideDependencyGateOutputRecord* dependencyGateOutputs; // +0x2C: 16B gate 输出表
  uint8_t              ukn_0x30[0x08];
  void*                visibilityByteOutputsHandle; // +0x38: +8 -> byte 权重输出缓冲
  RenderOverrideCompactScalarOutputEntry* compactScalarOutputs; // +0x3C: 4B stride 紧凑标量输出
}; // sizeof >= 0x40

struct RenderStagePresetOverrideNode {
  uint8_t  ukn_0x00[0xA8];
  uint32_t sourceRecordIndex;   // +0xA8: source point/vector/light 记录索引，node_type 1/2/7 都会消费它
  uint32_t outputSlotIndex;     // +0xAC: 目标输出槽/部件索引
  uint8_t  nodeType;            // +0xB0: 节点类型码
  uint8_t  modeBits;            // +0xB1: 模式位（0x38/0x40 等）
  uint8_t  ukn_0xB2[0x06];
  uint32_t childCount;          // +0xB8
  void*    childArray;          // +0xBC: 子节点数组
  uint8_t  ukn_0xC0[0x04];
  uint8_t  visibilityMaskIndex; // +0xC4: 0xFF=直接可见，否则查状态掩码表
}; // sizeof >= 0xC5

struct ParticleEmitterOverrideSlot;
struct PlaneParticleEmitterOverrideSlot;
struct CAnimRibbonObjStatus;
using AnimRibbonOverrideSlot = CAnimRibbonObjStatus;

// controller+0x44 指向的 part sequence / frame 定义头。
struct CModelPartSequenceDefHeader {
  uint8_t  ukn_0x00[0x18];
  void*    sequenceFrameRecords; // +0x18: stride 0x8C
  uint32_t* wrappedFramePeriods; // +0x20: per-channel period/modulus 表
}; // sizeof >= 0x24

// controller+0x08 指向的 16 字节当前帧窗口记录。
struct CModelPartFrameWindowRecord {
  int32_t currentFrameIndex;     // +0x00: 当前解析出的 frame/segment 索引
  int32_t frameStateWithLoopBit; // +0x04: bit31=loop/wrap 命中
  void*   onPartLoopOrComplete;  // +0x08: per-part 回调
  void*   onPartLoopOrCompleteCtx;// +0x0C
}; // sizeof = 0x10

// stride 0x8C 的序列帧定义记录。当前只确认前后界和 loop 标志。
struct CModelPartSequenceFrameRecord {
  int32_t frameStart;            // +0x00
  int32_t frameEnd;              // +0x04
  uint8_t ukn_0x08[0x04];
  uint32_t flags;                // +0x0C: bit0=loop 时按 clamp；否则按 wrap
  uint8_t ukn_0x10[0x7C];
}; // sizeof = 0x8C

// stride 0xE4 的部件状态定义记录。
struct CModelPartStateDefRecord {
  uint8_t ukn_0x00[0xE0];
  uint8_t visibilityDependencyIndex; // +0xE0: 0xFF=直接可见，否则查 dependency flags
  uint8_t ukn_0xE1[0x03];
}; // sizeof = 0xE4

// stride 0x38 的部件权重/可见性输出记录。
struct CModelPartWeightVisibilityRecord {
  uint8_t ukn_0x00[0x34];
  float   resolvedWeight;        // +0x34: >0 才允许参与递归/构建/提交
}; // sizeof = 0x38

// stride 0x1C 的依赖状态记录。
struct CModelVisibilityDependencyRecord {
  uint8_t  ukn_0x00[0x18];
  uint32_t flags;                // +0x18: bit0=dependency ready/visible
}; // sizeof = 0x1C

// CModel +0x98 的 part-state controller。当前只补高置信度字段。
struct CModelPartStateController {
  uint8_t                       ukn_0x00[0x08];
  CModelPartFrameWindowRecord*  frameWindowArray;      // +0x08: stride 0x10
  uint8_t                       ukn_0x0C[0x30];
  void*                         globalLoopCallback;    // +0x3C
  void*                         globalLoopCallbackCtx; // +0x40
  CModelPartSequenceDefHeader*  sequenceDefHeader;     // +0x44
  float                         frameTimeScale;        // +0x48
  uint32_t                      lastTickMs;            // +0x4C
  int32_t                       frameDelta;            // +0x50
  uint32_t                      flags;                 // +0x54
  uint8_t                       currentPartIndex;      // +0x58
  uint8_t                       ukn_0x59[0x03];
  uint32_t*                     wrappedFrameOffsets;   // +0x5C
  uint32_t                      wrappedFrameOffsetCount; // +0x60
  uint8_t                       ukn_0x64[0x30];
  ParticleEmitterOverrideSlot*  particleEmitterSlots;  // +0x94
  uint32_t                      particleEmitterSlotCount; // +0x98
  PlaneParticleEmitterOverrideSlot* planeParticleSlots;   // +0x9C
  uint32_t                      planeParticleSlotCount;   // +0xA0
  CAnimRibbonObjStatus*         animRibbonStatuses;    // +0xA4
  uint32_t                      animRibbonStatusCount; // +0xA8
}; // sizeof >= 0xAC

struct CModelArrayHeader {
  uint32_t capacity;            // +0x00
  uint32_t count;               // +0x04
  void*    data;                // +0x08
  uint32_t growthAlignment;     // +0x0C
}; // sizeof = 0x10

struct InlineVec3Array4 {
  uint32_t capacity;            // +0x00
  uint32_t count;               // +0x04
  void*    data;                // +0x08
  float    inlineVec3[12];      // +0x0C = 4 * (x,y,z)
}; // sizeof = 0x3C

struct CGeosetUvLayerRecord {
  uint32_t capacity;            // +0x00
  uint32_t count;               // +0x04
  void*    data;                // +0x08
  float    inlineVec2[8];       // +0x0C = 4 * (u,v)
}; // sizeof = 0x2C

struct CInlineUvLayerRecordArray1 {
  uint32_t             capacity;    // +0x00
  uint32_t             count;       // +0x04
  CGeosetUvLayerRecord* data;       // +0x08
  CGeosetUvLayerRecord inlineRecord;// +0x0C
}; // sizeof = 0x38

struct CGeosetPrimitiveRecord {
  uint32_t primitiveTypeOrMaterialSlot; // +0x00
  uint32_t indexCount;                  // +0x04
}; // sizeof = 0x08

struct CInlinePrimitiveRecordArray1 {
  uint32_t               capacity;      // +0x00
  uint32_t               count;         // +0x04
  CGeosetPrimitiveRecord* data;         // +0x08
  CGeosetPrimitiveRecord inlineRecord;  // +0x0C
}; // sizeof = 0x14

struct CInlineU16Array4 {
  uint32_t capacity;            // +0x00
  uint32_t count;               // +0x04
  uint16_t* data;               // +0x08
  uint16_t inlineValues[4];     // +0x0C
}; // sizeof = 0x14

struct CModelGeosetBindingRecord {
  int32_t layoutOrSlotA;        // +0x00 默认 -1
  int32_t layoutOrSlotB;        // +0x04 默认 -1
  float   scaleOrWeightA;       // +0x08 默认 1.0
  float   scaleOrWeightB;       // +0x0C 默认 1.0
}; // sizeof = 0x10

struct CGeosetData {
  void*                       vtable;               // +0x00
  uint32_t                    refCount;             // +0x04
  InlineVec3Array4            positions;            // +0x08
  CModelArrayHeader           vertexGroupIndices;   // +0x44 -> uint8_t，每顶点一个 matrix-group slot
  InlineVec3Array4            normals;              // +0x50
  CInlineUvLayerRecordArray1  uvLayers;             // +0x8C
  CInlinePrimitiveRecordArray1 primitiveRecords;    // +0xC4
  CInlineU16Array4            indicesU16;           // +0xD8
  uint32_t                    matrixGroupCapacity;  // +0xEC
  uint32_t                    matrixGroupCount;     // +0xF0
  uint32_t*                   matrixGroupSizes;     // +0xF4
  uint32_t                    matrixIndexCapacity;  // +0xF8
  uint32_t                    totalMatrixIndexCount;// +0xFC
  uint32_t*                   matrixIndices;        // +0x100
  uint32_t                    layoutOrMaterialMeta0;// +0x104
  uint32_t                    layoutOrMaterialSlot; // +0x108
  uint8_t                     ukn_0x10C[0x10];
  uint32_t                    mergedGeosetSlot;     // +0x11C 131320 写入
  uint8_t                     ukn_0x120[0x08];
}; // sizeof >= 0x128

struct CGeoset {
  void*      vtable;           // +0x00
  uint32_t   refCount;         // +0x04
  int32_t    handleId;         // +0x08
  CGeosetData* geosetData;     // +0x0C
  uint32_t   materialOrLayoutSlot; // +0x10 1325E0 从 source+0x10 复制
  uint8_t    ukn_0x14[0x04];
}; // sizeof >= 0x18

struct CModelData {
  void*             vtable;                // +0x00
  uint32_t          refCount;              // +0x04
  CModelArrayHeader geosets;               // +0x08 -> HGEOSET*
  CModelArrayHeader geosetBindings;        // +0x18 -> CModelGeosetBindingRecord
  CModelArrayHeader materials;             // +0x28 -> HMATERIAL*
  uint8_t           ukn_0x38[0x10];
  CModelArrayHeader materialLayoutBytes;   // +0x48 -> uint8_t / slot table
  CModelArrayHeader extraResourceHandles;  // +0x58
  uint8_t           ukn_0x68[0x2C];
  uint32_t          flags;                 // +0x94 bit0x10=complex runtime path
  void*             partStateTemplate;     // +0x98
  void*             modelDataHandle;       // +0x9C
  uint8_t           ukn_0xA0[0x18];
  uint32_t          lightCount;            // +0xB8
  CGxuLight**       lightDefs;             // +0xBC
  uint8_t           ukn_0xC0[0x08];
  void*             childRuntimeGroupRecords; // +0xC8 12B stride 组记录表
  uint8_t           ukn_0xCC[0x04];
  uint32_t          childRuntimeGroupCount;   // +0xD0
  uint8_t           ukn_0xD4[0x10];
  uint32_t          planeEmitterCount;     // +0xE8
  void**            planeEmitterDefs;      // +0xEC
  uint8_t           ukn_0xF0[0x10];
  uint32_t          cameraLikeCount;       // +0x100
  void**            cameraLikeDefs;        // +0x104
}; // sizeof >= 0x108

struct CModel {
  void*                       vtable;               // +0x00
  uint32_t                    refCount;             // +0x04
  CModelArrayHeader           runtimeGeosets;       // +0x08
  CModelArrayHeader           geosetBindingRecords; // +0x18
  CModelArrayHeader           runtimeMaterials;     // +0x28
  uint8_t                     ukn_0x38[0x10];
  CModelArrayHeader           runtimeExtraHandles;  // +0x48
  uint32_t                    finalPoseMatrixCount; // +0x5C
  GxStagePresetRecord*        finalPoseMatrixArray; // +0x60 stride 0x30 的 3x4 palette
  float                       currentWorldMatrix3x4[12]; // +0x64
  uint32_t                    flags;                // +0x94 bit2=non-unit-scale bit4=complex tree
  CModelPartStateController*  partStateController;  // +0x98
  CModelData*                 modelData;            // +0x9C
  uint8_t                     ukn_0xA0[0x24];
  uint32_t                    childBucketCount;     // +0xC4
  SceneNodeChildBucket*       childBucketArray;     // +0xC8
  uint8_t                     ukn_0xCC[0x08];
  uint8_t*                    childVisibilityCache; // +0xD4
}; // sizeof >= 0xD8

// override graph node_type=4 使用的 0x80 slot 记录。
// 这不是最终粒子对象，而是“可见性门控 + CParticleEmitterRuntime”之间的桥接状态。
struct ParticleEmitterOverrideSlot {
  uint8_t  ukn_0x00[0x70];
  uint8_t  gateResolveState[0x0C]; // +0x70: sub_6F77A940 读取的门控/可见性状态块
  float    emissionScale;          // +0x7C: 传给 CParticleEmitterRuntime_UpdateAndRender
}; // sizeof = 0x80

// override graph node_type=5 使用的 0x8C slot 记录。
// 真正的 CPlaneParticleEmitter 对象通过 +0x88 指针跳转。
struct PlaneParticleEmitterOverrideSlot {
  uint8_t  ukn_0x00[0x58];
  uint8_t  gateResolveState[0x30]; // +0x58: sub_6F77D350 读取的门控/权重状态块
  void*    planeParticleEmitter;   // +0x88: CPlaneParticleEmitter*
}; // sizeof = 0x8C

// 44 字节的 CGxuLight。COmniLight 和 override graph node_type=1 都会复用它。
struct CGxuLight {
  uint32_t stateFlag;               // +0x00: sub_6F0CC5F0 直接返回它
  uint32_t useInputPointTransform;  // +0x04: 1 时直接吃 override graph 传入点位
  float    positionOrDirection[3];  // +0x08
  uint32_t ambientColorPacked;      // +0x14: RGB bytes
  uint32_t directionalColorPacked;  // +0x18: RGB bytes
  float    ambientIntensity;        // +0x1C
  float    directionalIntensity;    // +0x20
  uint32_t refCount;                // +0x24
  float    maxDistanceOrRange;      // +0x28: ctor 默认 +INF
}; // sizeof = 0x2C

// override graph node_type=7 写到 runtime+0xB4 的固定 0x40 输出槽。
struct RenderOverrideLocalPointOutputRecord {
  uint8_t ukn_0x00[0x34];
  float   resolvedLocalPoint[3];    // +0x34: 当前 scratch 3x4 变换后的局部点位
}; // sizeof = 0x40

// override graph node_type=6 实际更新的是 0x74 的 CAnimRibbonObjStatus。
// +0x70 指向真正的 0x16C CAnimRibbonObj。
struct CAnimRibbonObjStatus {
  uint8_t  ukn_0x00[0x28];
  uint8_t  gateResolveState[0x28]; // +0x28: sub_6F77A940 读取的门控/可见性状态块
  uint8_t  ukn_0x50[0x20];
  void*    animRibbonObj;          // +0x70: CAnimRibbonObj*
}; // sizeof = 0x74

struct CParticleEmitterRuntimeParticle {
  float    remainingLife;          // +0x00
  float    spawnPhaseOffset;       // +0x04
  float    position[3];            // +0x08
  float    velocity[3];            // +0x14
  uint32_t renderScaleOrSize;      // +0x20: 传给 sub_6F12F270 的标量
  void*    modelInstanceOrSprite;  // +0x24: sub_6F12EE90 / sub_6F12F270 使用
}; // sizeof = 0x28

// node_type=4 实际驱动的 0x68 运行时对象。它维护 active/free index 栈并逐粒子推进生命周期。
struct CParticleEmitterRuntime {
  uint8_t                         ukn_0x00[0x04];
  float                           spawnAccumulator; // +0x04
  void*                           spawnCtxA;        // +0x08: 非空门控之一
  void*                           spawnCtxB;        // +0x0C: 非空门控之一
  float                           emissionRateScale;// +0x10: 参与 spawn count 累加
  uint8_t                         ukn_0x14[0x24];
  CParticleEmitterRuntimeParticle* particleArray;   // +0x38: stride 0x28
  uint8_t                         ukn_0x3C[0x0C];
  uint32_t*                       activeIndices;    // +0x48
  uint8_t                         ukn_0x4C[0x04];
  uint32_t                        activeCount;      // +0x50
  uint8_t                         ukn_0x54[0x08];
  uint32_t*                       freeIndices;      // +0x5C
  uint8_t                         ukn_0x60[0x04];
  uint32_t                        freeCount;        // +0x64
}; // sizeof = 0x68

// CPlaneParticleEmitter 的真实对象由指针数组管理；当前只确认 flags 和 3x4 矩阵落在对象后半段。
struct CPlaneParticleEmitter {
  uint8_t  ukn_0x00[0x194];
  uint32_t flags;                 // +0x194: bit0/bit6/bit7/bit11 参与启停与矩阵更新
  float    worldMatrix3x4[12];    // +0x198
}; // sizeof >= 0x1C8

// CAnimRibbonObj 是连续的 0x16C 数组对象；CAnimRibbonObjStatus 只负责门控和桥接它。
struct CAnimRibbonObj {
  uint8_t  ukn_0x00[0x18];
  uint32_t lastTickMs;            // +0x18: GetTickCount 基准
  float    phaseAccumulator;      // +0x1C: 周期/相位累计
  uint32_t hasCachedTransform;    // +0x20
  uint8_t  ukn_0x24[0x12C];
  uint32_t flags;                 // +0x150: bit0=enabled, bit1=ready
  uint8_t  ukn_0x154[0x10];
  int32_t  ukn_0x164;             // +0x164: ctor 默认 -1
  int32_t  ukn_0x168;             // +0x168: ctor 默认 1
}; // sizeof = 0x16C

// 36 字节的 layer state 记录。前 20 字节会参与 memcmp 排序/复用判断。
struct MeshLayerStateRecord {
  uint32_t primary_resource_binding; // +0x00: 传给主资源绑定 helper 的 layer 级主绑定对象
  uint32_t state_words_0x04[5];  // +0x04 ~ +0x17: ApplyStateBlock 比较/提交主区
  uint32_t blend_or_draw_mode;   // +0x18: special alpha 路径会判断是否等于 4
  uint32_t aux_ref_enable_0;     // +0x1C: 配合 dispatch record +0x14
  uint32_t aux_ref_enable_1;     // +0x20: 配合 dispatch record +0x18
}; // sizeof = 0x24

// 44 字节的 layer dispatch 记录。
struct MeshLayerDispatchRecord {
  uint8_t             ukn_0x00[0x0C];
  int32_t             stage_preset_index_0; // +0x0C: stage slot0 选中的共享 preset
  int32_t             stage_preset_index_1; // +0x10: stage slot1 选中的共享 preset
  int32_t             aux_ref_index_0;    // +0x14: 额外 stage/sampler 引用索引
  int32_t             aux_ref_index_1;    // +0x18: 第二个额外引用索引
  uint32_t            visibility_offset;  // +0x1C: SceneNode +0x50 可见性表索引
  uint32_t            alpha_flags;        // +0x20: bit0 控制双 alpha/color 提交
  uint32_t            stage_mode_0;       // +0x24: >=12 时 stage0 走共享 preset
  uint32_t            stage_mode_1;       // +0x28: >=12 时 stage1 走共享 preset
}; // sizeof = 0x2C

struct MeshAuxResourceEntry {
  uint8_t             ukn_0x00[0x08];
  uint32_t            resource_binding;   // +0x08: 供 aux_ref_index_* 查出的真实资源绑定
  uint8_t             ukn_0x0C[0x20];
}; // sizeof = 0x2C

// ============================================================================
// MeshData结构（网格数据）
// ============================================================================

struct MeshData {
  void *vtable;                 // +0x00: 虚函数表
  uint8_t padding1[0x08];       // +0x04
  uint32_t primary_stream_arg0; // +0x0C: 传给多槽流绑定 helper 的主流参数0
  uint32_t primary_stream_ptr;  // +0x10: 主流顶点数据基址/绑定指针
  uint8_t padding14[0x34];      // +0x14
  uint32_t primary_stream_stride; // +0x48: 主流 stride
  uint32_t stream1_ptr;         // +0x4C: 第二个流/属性源指针
  uint8_t padding50[0x08];      // +0x50
  uint32_t stream1_stride;      // +0x58: 第二个流/属性源 stride
  uint8_t padding5C[0x38];      // +0x5C
  MeshAuxResourceEntry *aux_layer_resource_table; // +0x94: 供 dispatch record +0x14/+0x18 查表
  uint8_t padding98[0x30];      // +0x98
  uint32_t sub_primitive_count; // +0xC8: fallback / special 子批次数
  void *sub_primitive_pairs;    // +0xCC: 每项 8 字节
  uint8_t paddingD0[0x10];      // +0xD0
  uint32_t primitive_base_index;// +0xE0: sub_6F0E3520 / 3550 的 base
  uint8_t paddingE4[0x0C];      // +0xE4
  void *transform_or_pose_ctx;  // +0xF0: UpdateItemWorldMatrix 用到的上下文
  uint8_t paddingF4[0x10];      // +0xF4
  uint32_t meshFlag;            // +0x104: 非 0 时走 special/快速路径
  uint32_t meshIndex;           // +0x108: SceneNode +0x30 的索引
  float boundingPos[3];         // +0x10C: 透明排序用包围盒中心
  uint8_t padding118[0x04];     // +0x118
  uint32_t cullIndex;           // +0x11C: SceneNode +0x20 的 16 字节记录索引
  uint32_t transparentKey;      // +0x120: 透明排序键
  uint32_t extraMeshFlags;      // +0x124: 额外 mesh 标志，bit2 影响 0x6F138510 过滤
}; // sizeof = 0x128

// ============================================================================
// MeshInfo结构（网格信息）
// ============================================================================

struct MeshInfo {
  uint8_t padding1[12];              // +0x00
  uint32_t layerCount;               // +0x0C
  MeshLayerStateRecord *layerStates; // +0x10: 36 字节 stride
  uint8_t padding2[36];              // +0x14
  void *layerInfo;                   // +0x38: 指向 layer dispatch 组
}; // sizeof >= 0x3C

// ============================================================================
// LayerInfo结构（层信息）
// ============================================================================

struct LayerInfo {
  uint8_t padding1[16];                  // +0x00
  MeshLayerDispatchRecord *layerRecords; // +0x10: 44 字节 stride
}; // sizeof = 0x14

// ============================================================================
// LayerData结构（层数据）
// ============================================================================

struct LayerData { // stride = 44 (0x2C)
  // 实际使用从 +0x1C 开始
  void *layerVisibilityPtr; // +0x00 (相对于layerDataBase+0x1C): 层可见性指针
  uint8_t padding1[24];     // +0x04 ~ +0x1B: 未知
  uint8_t layerFlags;       // +0x08 (相对): 标志位（bit0影响颜色处理）
                            // ...
}; // sizeof = 44 bytes

// ============================================================================
// LayerState结构（层状态）
// ============================================================================

struct LayerState { // stride = 36 (0x24)
  // 状态块基址 +4 开始
  // 用于memcmp比较判断layerChanged（仅比较前20字节）
  // 传递给GxDevice_ApplyStateBlock
  uint8_t data[36];
}; // sizeof = 36 bytes

// ============================================================================
// CullEntry结构（剔除表条目）
// ============================================================================

struct CullEntry {      // stride = 16 (0x10)
  uint8_t padding1[3];  // +0x00 ~ +0x02: 未知
  uint8_t visible;      // +0x03: 可见标志（0=不可见）
  uint8_t padding2[12]; // +0x04 ~ +0x0F: 未知
}; // sizeof = 16 bytes

// ============================================================================
// 全局变量
// ============================================================================

namespace global {
// 渲染队列全局变量
extern uint32_t g_RenderQueue_BatchCapacity;       // @ 0xBC6BA8
extern uint32_t g_RenderQueue_NumOfElements;       // @ 0xBC6BAC
extern void *g_RenderQueue_BatchArray;             // @ 0xBC6BB0
extern uint32_t g_RenderQueue_SortedCount;         // @ 0xBC6BA0 (max 10000)
extern uint32_t g_RenderQueue_BatchGrowStep;       // @ 0xBC6BB4
extern void **g_RenderQueue_SortedPtrs;            // @ 0xBC6BE8
extern uint32_t g_RenderQueue_StateOptEnabled;     // @ 0xBDA4D0
extern uint32_t g_RenderQueue_StateCleanupPending; // @ 0xBDA4D4
extern uint8_t *g_RenderQueue_StageInitialized;    // @ 0xBDA4D8
extern uint32_t g_RenderQueue_StageCount;          // @ 0xBDA4E0
extern uint32_t g_RenderQueue_StageCountInit;      // @ 0xBDA4E4

// 透明队列全局变量
extern uint32_t g_AUCTransparent_Capacity;    // @ 0xBC6BB8
extern uint32_t g_AUCTransparent_Count;       // @ 0xBC6BBC
extern void *g_AUCTransparent_Array;          // @ 0xBC6BC0
extern uint32_t g_AUCTransparent_GrowStep;    // @ 0xBC6BC4
extern uint32_t g_AUCTransparent_SortedCount; // @ 0xBC6BA4 (max 10000)
extern void **g_AUCTransparent_SortedPtrs;    // @ 0xBD0828

// 相机全局变量
extern float g_RenderCamera_PosXY[2]; // 相机XY位置
extern float g_RenderCamera_PosZ;     // 相机Z位置

// TeamColor系统
extern uint32_t g_TeamColorCount;  // @ 0xBE6184 (=24)
extern void **g_TeamColorTextures; // @ 0xBE6188
extern uint32_t g_TeamColorFriend; // @ 0xBE62F0
extern uint32_t g_TeamColorEnemy;  // @ 0xBE62FC
} // namespace global

// GxDevice全局实例
extern GxDevice *gx_device; // @ 0xBC5420

// ============================================================================
// 外部函数声明（C linkage）
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// List操作函数
void *List_GetData(ListHeader *list);     // 返回 list[12]
uint32_t List_GetCount(ListHeader *list); // 返回 list[20]

// 渲染队列函数
void WAR3_NATIVE_CB RenderQueue_AddBatch(int sceneNode, int reserved);
void RenderQueue_FlushAndReset(void);

// 批次提交函数
void RenderBatch_Submit(SceneNode *sceneNode);

// 场景节点透明列表添加
void SceneNode_AddTransparentList0(SceneNode *node, void *world); // 粒子
void SceneNode_AddTransparentList2(SceneNode *node, void *world); // 缎带
void SceneNode_AddTransparentList3(SceneNode *node, void *world); // 特效
void SceneNode_AddTransparentList4(SceneNode *node, void *world); // 附着物

// 地形渲染函数
void Terrain_RenderStage(int terrainType); // sub_6F76F060

// 状态设置函数（原版两套缓存状态机）
void WAR3_NATIVE_CB RenderCategory_Enable(CWorldFrameWar3 *world, int reserved,
                                          RenderCategoryMask mask,
                                          RenderCategoryMask current); // 0x6F0A1400
void WAR3_NATIVE_CB RenderCategory_Disable(CWorldFrameWar3 *world, int reserved,
                                           RenderCategoryMask mask,
                                           RenderCategoryMask current); // 0x6F0A2800
void WAR3_NATIVE_CB ApplyCategoryMode(CWorldFrameWar3 *world, int reserved,
                                      int mode,
                                      int flag); // 0x6F363350
void StateCleanup(void *cleanupPtr);            // 0x6F185450

// RenderScene 主链辅助函数
void RenderStage0_PreRenderContext(void *context); // 0x6F186300
void CWorld_SetShadowMode(int handle, int enabled); // 0x6F76F550
void CWorld_ToggleGroup1ShadowPass(CWorldFrameWar3 *world,
                                   int enabled); // 0x6F3621E0
bool CWorld_HasPostProcessPreview();             // 0x6F3597C0
void CWorld_CommitPostProcessQueue();            // 0x6F3ACFF0
void CWorld_RenderPreviewContext(void *context); // 0x6F3C4330
int CWorld_RenderSelectionManagerStage();        // 0x6F367980
void CWorld_RenderStage21IndicatorTail(void *context); // 0x6F76F190

// 调试/编辑器函数
void DebugRender_CheckFlags(uint32_t flags);         // sub_6F368A90
void RenderSceneCleanupContext_Flush(void *context); // sub_6F3ACCF0

// 可见性检查函数
int Visibility_Check(void *context, int index); // sub_6F777FE0

// 辅助工具函数
void TransformPoint3x4(float *result, const float *point,
                       const float *matrix);

// RenderQueue核心函数
int RenderQueue_ItemComparator(const void *a, const void *b);
bool RenderQueue_ItemLess(const RenderBatchElement *a,
                         const RenderBatchElement *b);
bool RenderBatch_CanEnqueueToMainQueue(SceneNode *sceneNode, void *part);
void AUCTransparent_AddEntry(void *part, uint32_t type,
                             const float *worldPos, uint32_t transparentKey);
unsigned int RenderQueue_FlushSortedItems(void);
void RenderQueue_StageUpdate(void *forceRefresh, int param_edi = 0,
                             int param_esi = 0);

// Dispatch函数（GPU指令派发）
int RenderQueue_Dispatch_Common(void *part, int layerChanged, int stateChanged);
int RenderQueue_Dispatch_Special(void *part, int stateChanged);

// 原版二进制签名（用于后续 takeover 设计，不要求当前 wrapper 完全同型）：
// - RenderBatch_CanEnqueueToMainQueue:
//   __fastcall(SceneNode* sceneNode, RenderablePart* part)
// - RenderQueue_Dispatch_Common:
//   __fastcall(SceneNode* sceneNode, RenderablePart* part,
//              int layerIndex, int stateChanged, int layerChanged)
// - RenderQueue_Dispatch_Special:
//   __fastcall(SceneNode* sceneNode, RenderablePart* part,
//              int layerIndex, int layerChanged)

// GxDevice函数（图形设备接口）
void GxDevice_ApplyStateBlock(void *stateBlock);
void GxDevice_StateCleanup74();
void GxDevice_StateCleanup78();
void GxDevice_RenderSceneFlush();
void GxDevice_SetVertexBuffer(int offset);
void GxDevice_DrawPrimitive();

// Native渲染函数
int WAR3_NATIVE_CB Native_CWorld_RenderScene(CWorldFrameWar3 *world, int reserved);
int WAR3_NATIVE_CB Native_RenderWorld_DispatchStage(
    CWorldFrameWar3 *world, int reserved, RenderStage stageId, CategoryMode category,
    RenderCategoryMask renderCategory, int unknown);
int WAR3_NATIVE_CB Native_WorldObjects_RenderGroup(
    CWorldFrameWar3 *world, int reserved, int categoryMode, WorldGroupIndex groupIdx);
int WAR3_NATIVE_CB Native_WorldObjectEntry_Render(int entry, int reserved);
void WAR3_NATIVE_CB Native_RenderQueue_AddBatch(SceneNode *sceneNode,
                                                int reserved, int categoryMode);

#ifdef __cplusplus
}
#endif

// ============================================================================
// MemHack 集成结构定义
// ============================================================================

// 字符串表示结构
struct CStringRep {
  void**              vf_table;      // 0x0
  uint32_t            ref_count;     // 0x4
  int32_t             string_hash;   // 0x8
  uint8_t             ukn_0xC[0x4];
  const CStringRep*   next_string;   // 0x10
  uint8_t             ukn_0x14[0x8];
  const char*         str;           // 0x1C
  
  const char* get_str() const { return str; }
};

// 相机结构
struct CCamera {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  uint8_t             ukn_0x8[0x34];
  float               camera_x;       // 0x3C
  float               camera_y;       // 0x40
  float               camera_z;       // 0x44
  uint8_t             ukn_0x48[0x1C];
  float               focus_x;        // 0x64
  float               focus_y;        // 0x68
  float               focus_z;        // 0x6C
};

// War3专用相机结构
struct CCameraWar3 {
  CCamera base;
  // 扩展字段待补充
};

// 光照结构 (基础)
struct CLight {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// UI 3D 宿主基类。此处只保留已确认尺寸和关键挂载字段。
struct CModelFrame {
  uint8_t             ukn_0x0[0x178];
}; // sizeof = 0x178

struct CSpriteFrame {
  uint8_t             ukn_0x0[0x178];
  CSpriteUber**       sprite;         // 0x178
  uint8_t             ukn_0x17C[0x34];
}; // sizeof = 0x1B0

struct CCursorFrame : public CSpriteFrame {
}; // sizeof = 0x1B0

// 游戏UI系统结构
struct CGameUI {
  void**              vf_table;       // 0x0
  uint8_t             ukn_0x4[0x28];
  CSpriteUber*        cursor_sprite;  // 0x2C
  uint8_t             ukn_0x30[0x13C];
  CCursorFrame*       cursor_frame;   // 0x16C
  uint8_t             ukn_0x170[0x3C];
  void*               postprocess_preview_primary;   // 0x1AC
  void*               postprocess_preview_secondary; // 0x1B0
  void*               cur_mode;       // 0x1B4 当前mode
  uint32_t            max_mode_count; // 0x1B8 最大mode数量
  uint32_t            cur_mode_count; // 0x1BC 当前mode数量
  void**              cur_select_mode;// 0x1C0 当前select_mode数组
  uint8_t             ukn_0x1C4[0x4];
  void*               tool_tip;      // 0x1C8
  void*               uber_tool_tip;  // 0x1CC
  uint8_t             ukn_0x1D0[0x40];
  void*               target_mode;   // 0x210
  void*               select_mode;   // 0x214
  void*               drag_select_mode;// 0x218
  void*               drag_scroll_mode;// 0x21C
  void*               build_mode;    // 0x220
  void*               signal_mode;   // 0x224
  void*               esc_menu;      // 0x228
  uint8_t             ukn_0x22C[0x4];
  void*               suspend_indicator;// 0x230
  void*               unresponsive_indicator;// 0x234
  void*               alliance_mode;  // 0x238
  void*               chat_mode;      // 0x23C
  void*               script_dialog_mode;// 0x240
  void*               quest_mode;     // 0x244
  uint8_t             ukn_0x248[0x4];
  void*               selection_manager;// 0x24C
  void*               drag_scroll_manager;// 0x250
  CCameraWar3*        camera_war3;    // 0x254
  uint8_t             ukn_0x258[0x154];
  void*               multi_board;   // 0x3AC
  uint8_t             ukn_0x3B0[0xC];
  CWorldFrameWar3*     world_frame_war3;// 0x3BC
  CMiniMap*           mini_map;       // 0x3C0
  CInfoBar*           info_bar;       // 0x3C4
  CCommandBar*        command_bar;    // 0x3C8
  CResourceBar*       resource_bar;   // 0x3CC
  CUpperButtonBar*     upper_button_bar;// 0x3D0
  void*               ukn_frame_0x3D4;
  void*               command_bar_cover;// 0x3D8 技能栏遮罩
  CHeroBar*           hero_bar;       // 0x3DC
  CPeonBar*           peon_bar;       // 0x3E0
  void*               error_message;   // 0x3E4
  void*               text_message;    // 0x3E8
  void*               chat_message;    // 0x3EC
  void*               upkeep_message;  // 0x3F0
  CPortraitButton*    portrait_button;// 0x3F4
  CTimeOfDayIndicator* time_indicator;// 0x3F8
  CChatEditBar*       chat_edit_bar;  // 0x3FC
  CCinematicPanel*    cinematic_panel;// 0x400
  uint8_t             ukn_0x404[4];
  void*               button_minimap_signal;// 0x408
  void*               button_minimap_terrain;// 0x40C
  void*               button_minimap_color;// 0x410
  void*               button_minimap_creep;// 0x414
  void*               button_formation;// 0x418
  void*               ukn_frame_0x41C;
  void*               ukn_frame_0x420;
  void*               ukn_frame_0x424;
  void*               console_ui;     // 0x428 ConsoleUI
  uint32_t            hotkey_quick_save;// 0x42C
  uint32_t            hotkey_quick_load;// 0x430
  uint32_t            hotkey_help;    // 0x434
  uint32_t            hotkey_option;  // 0x438
  uint32_t            hotkey_quit_game;// 0x43C
  uint32_t            hotkey_minimap_signal;// 0x440
  uint32_t            hotkey_minimap_terrain;// 0x444
  uint32_t            hotkey_minimap_color;// 0x448
  uint32_t            hotkey_minimap_creep;// 0x44C
  uint32_t            hotkey_formation;// 0x450
}; // sizeof = 0x454

// 小地图结构
struct CMiniMap {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 信息栏结构
struct CInfoBar {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  uint8_t             ukn_0x8[0x124];
  void*               cur_frame;      // 0x12C
  void*               unit_detail;    // 0x130
  void*               building_detail;// 0x134
  void*               cargo_detail;   // 0x138
  void*               group_detail;  // 0x13C
  void*               item_detail;    // 0x140
  void*               destructable_detail;// 0x144
  void*               inventory_bar;  // 0x148
  void*               inventory_cover;// 0x14C
  void*               inventory_text;// 0x150
};

// 命令栏结构
struct CCommandBar {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 资源栏结构
struct CResourceBar {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 上方按钮栏结构
struct CUpperButtonBar {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 英雄栏结构
struct CHeroBar {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 农民栏结构
struct CPeonBar {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 头像按钮结构
struct CPortraitButton {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  uint8_t             ukn_0x8[0x4];
  uint32_t            flag_0xC;      // 0xC
  uint8_t             ukn_0x10[0x130];
  CCamera*            camera;        // 0x140
  uint8_t             ukn_0x144[0xF4];
  CUnit*              portrait_unit;  // 0x238
  uint32_t            portrait_unit_id;// 0x23C
  void*               hp_text;       // 0x240
  void*               mp_text;       // 0x244
  float               cur_hp;        // 0x248
  float               max_hp;        // 0x24C
  float               cur_mp;        // 0x250
  float               max_mp;        // 0x254
  uint8_t             ukn_0x258[0xC];
}; // sizeof = 0x264

// 时间指示器结构
struct CTimeOfDayIndicator {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 聊天编辑栏结构
struct CChatEditBar {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 电影面板结构
struct CCinematicPanel {
  void**              vf_table;       // 0x0
  // 详细字段待补充
};

// 单位结构 (简化版)
struct CUnit {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  uint32_t            ukn_0x8;
  uint32_t            hash_0xC;      // 0xC
  uint32_t            hash_0x10;     // 0x10
  uint8_t             ukn_0x14[0xC];
  uint32_t            flag_0x20;     // 0x20
  uint8_t             ukn_0x24[0x4];
  CSprite*            sprite;        // 0x28
  uint8_t             ukn_0x2C[0x4];
  uint32_t            id;            // 0x30
  uint8_t             ukn_0x34[0x1C];
  void*               preselect_ui;  // 0x50
  uint32_t            stun_count;    // 0x54 眩晕计数器
  uint32_t            owner_id;      // 0x58
  // ... 更多字段待补充
}; // sizeof = 0x30C+

// CWidget 公共前缀：CreateAttachedEffect / GetSprite 路径已确认。
struct CWidget {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  uint32_t            ukn_0x8;       // 0x8
  uint32_t            hash_0xC;      // 0xC
  uint32_t            hash_0x10;     // 0x10
  uint8_t             ukn_0x14[0xC];
  uint32_t            flag_0x20;     // 0x20
  uint8_t             ukn_0x24[0x4];
  CSprite*            sprite;        // 0x28
  uint8_t             ukn_0x2C[0x4];
  uint32_t            type_id;       // 0x30
}; // sizeof = 0x34

struct AnimationSequenceDescriptor {
  uint32_t            user_flags_or_group; // 0x00
  const char*         name;                // 0x04 -> AnimationSequenceData + 0x3C
  uint32_t            duration_ms;         // 0x08
  float               move_speed;          // 0x0C
  uint32_t            priority;            // 0x10
  float               rarity;              // 0x14
  uint32_t            flags;               // 0x18
  int32_t             sub_animation_index; // 0x1C
}; // sizeof = 0x20

using AnimSequenceInfo = AnimationSequenceDescriptor;

struct AnimationSequenceData {
  uint32_t            interval_start_ms;   // 0x00
  uint32_t            interval_end_ms;     // 0x04
  float               move_speed;          // 0x08
  uint32_t            flags;               // 0x0C
  uint32_t            priority;            // 0x10
  float               rarity;              // 0x14
  uint8_t             ukn_0x18[0x24];
  char                name[0x50];         // 0x3C
}; // sizeof = 0x8C

struct AnimationSequenceLookupEntry {
  uint8_t             ukn_0x0[0x8];
  uint8_t             internal_sequence_id; // 0x8
  uint8_t             ukn_0x9[0x3];
}; // sizeof = 0x0C

struct CAnimSequenceProvider {
  uint8_t                     ukn_0x0[0x8];
  AnimationSequenceLookupEntry* external_to_internal_map; // 0x08
  uint32_t                    external_sequence_count;    // 0x0C
  uint8_t                     ukn_0x10[0x8];
  AnimationSequenceData*      sequence_data;              // 0x18
  uint32_t                    sequence_count;             // 0x1C
}; // sizeof = 0x20

struct AnimationTimeState {
  uint32_t            stamp;               // 0x00
  uint32_t            flags;               // 0x04
  uint8_t             ukn_0x8[0x8];
}; // sizeof = 0x10

struct CAnimComplex {
  uint8_t                 ukn_0x0[0x8];
  AnimationTimeState*     time_states;         // 0x08
  uint32_t                sequence_count;      // 0x0C
  uint8_t                 ukn_0x10[0x2C];
  void*                   on_sequence_end_callback; // 0x3C
  void*                   on_sequence_end_context;  // 0x40
  CAnimSequenceProvider*  sequence_provider;   // 0x44
  float                   time_scale;          // 0x48
  uint8_t                 ukn_0x4C[0x8];
  uint32_t                flags;               // 0x54
  uint8_t                 current_sequence_index; // 0x58
  uint8_t                 ukn_0x59[0x17];
  uint32_t*               bone_blend_buffer;   // 0x70
  uint32_t                bone_blend_count;    // 0x74
  uint8_t                 ukn_0x78[0x6C];
  uint32_t                blend_transition_time; // 0xE4
}; // sizeof = 0xE8

struct CModelAnimController {
  void**                  vf_table;            // 0x00
  uint8_t                 ukn_0x4[0x94];
  CAnimComplex*           anim_complex;        // 0x98
}; // sizeof = 0x9C

struct CSpriteAnimPayloadArray {
  uint32_t            capacity;       // 0x0
  uint32_t            size;           // 0x4
  void*               data;           // 0x8
  uint32_t            growth_alignment; // 0xC
}; // sizeof = 0x10

struct CSpriteAnimRequest {
  uint32_t            request_flags;  // 0x0
  int32_t             request_key_or_sentinel; // 0x4
  CSpriteAnimPayloadArray candidate_sequences; // 0x8
  uint32_t            inline_sequence_slot1;   // 0x18
}; // sizeof = 0x1C

// 精灵基础结构：动画请求环形队列 + 挂点附着状态。
struct CSprite {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  uint8_t             ukn_0x8[0x18];
  CModelAnimController* model_runtime; // 0x20
  uint32_t            ukn_0x24;      // 0x24
  uint32_t            sprite_flags;  // 0x28
  int32_t             cached_sequence_id; // 0x2C
  void*               sequence_provider; // 0x30
  uint8_t             anim_queue_count; // 0x34
  uint8_t             anim_queue_head; // 0x35
  uint8_t             anim_queue_tail; // 0x36
  uint8_t             ukn_0x37;      // 0x37
  uint32_t            anim_queue_storage_hint; // 0x38
  uint32_t            anim_queue_capacity; // 0x3C
  CSpriteAnimRequest* anim_queue_entries; // 0x40
}; // sizeof = 0x44

// 精灵 Uber 结构：补上 prerender 里高置信度命中的字段。
struct CSpriteUber : public CSprite {
  uint8_t             ukn_0x44[0x50];
  uint32_t            anim_time_override_enabled; // 0x94
  uint8_t             ukn_0x98[0x8];
  float               anim_time_override_sec; // 0xA0
  uint8_t             ukn_0xA4[0x1C];
  float               x;             // 0xC0
  float               y;             // 0xC4
  float               z;             // 0xC8
  uint8_t             ukn_0xCC[0x1C];
  float               uniform_scale; // 0xE8
  uint8_t             ukn_0xEC[0x1C];
  float               world_matrix_3x4[12];  // 0x108
  uint32_t            render_flags_0x138; // 0x138
  uint8_t             ukn_0x13C[0xC];
  uint32_t            color_rgba;    // 0x148
};

struct CModelInstance {
  void**              vf_table;             // 0x0
  uint8_t             ukn_0x4[0x1C];
  void*               renderable;           // 0x20
  void*               parent_bone_system;   // 0x24
  uint32_t            instance_flags;       // 0x28
  uint8_t             ukn_0x2C[0x2];
  int16_t             attached_point_index; // 0x2E
}; // sizeof = 0x30

struct HashGroup {
  uint32_t            hash_0x0;       // 0x0
  uint32_t            hash_0x4;       // 0x4

  HashGroup(uint32_t hash1 = 0xFFFFFFFF, uint32_t hash2 = 0xFFFFFFFF)
    : hash_0x0(hash1)
    , hash_0x4(hash2)
  {}

  bool is_exist() { return !!this; }

  bool is_valid() {
    return (is_exist() && ((int32_t)hash_0x0 > 0) &&
            ((int32_t)hash_0x4 > 0));
  }

  bool operator==(const HashGroup& rhs) const {
    return hash_0x0 == rhs.hash_0x0 && hash_0x4 == rhs.hash_0x4;
  }
};

struct CEffect {
  void**              vf_table;           // 0x0
  void*               ref_count;          // 0x4
  uint8_t             ukn_0x8[0x4];
  int32_t             hash_0xC;           // 0xC
  int32_t             hash_0x10;          // 0x10
  uint8_t             ukn_0x14[0xC];
  uint32_t            attach_flags;       // 0x20
  uint8_t             ukn_0x24[0x4];
  CModelInstance*     model_instance;     // 0x28
  uint8_t             ukn_0x2C[0x20];
  uint32_t            attach_point_count; // 0x4C
  uint32_t            attach_point_ids[10]; // 0x50
  HashGroup           bound_agent_hash;   // 0x78
}; // sizeof = 0x80

struct CEffectFloating : public CEffect {
  uint8_t             ukn_0x80[0x4];
  int16_t             floating_anim_state; // 0x84
};

using CAttachedEffect = CEffect;
using CAttachedEffectStatic = CEffect;
using CAttachedEffectFloating = CEffectFloating;
static_assert(sizeof(CWidget) == 0x34, "CWidget 大小不匹配");
static_assert(sizeof(CSpriteAnimPayloadArray) == 0x10,
              "CSpriteAnimPayloadArray 大小不匹配");
static_assert(sizeof(CSpriteAnimRequest) == 0x1C,
              "CSpriteAnimRequest 大小不匹配");
static_assert(sizeof(CModelAnimController) == 0x9C,
              "CModelAnimController 大小不匹配");
static_assert(sizeof(CSprite) == 0x44, "CSprite 大小不匹配");
static_assert(sizeof(CModelInstance) == 0x30, "CModelInstance 大小不匹配");
static_assert(sizeof(CEffect) == 0x80, "CEffect 大小不匹配");
static_assert(sizeof(CModelFrame) == 0x178, "CModelFrame 大小不匹配");
static_assert(sizeof(CSpriteFrame) == 0x1B0, "CSpriteFrame 大小不匹配");
static_assert(sizeof(CCursorFrame) == 0x1B0, "CCursorFrame 大小不匹配");
static_assert(sizeof(CPortraitButton) == 0x264, "CPortraitButton 大小不匹配");
static_assert(sizeof(SceneNodeChildLink) == 0x10,
              "SceneNodeChildLink 大小不匹配");
static_assert(sizeof(SceneNodeChildBucket) == 0x0C,
              "SceneNodeChildBucket 大小不匹配");
static_assert(sizeof(RenderBatchElement) == 0x14,
              "RenderBatchElement 大小不匹配");
static_assert(sizeof(SceneNodeTintRecord) == 0x10,
              "SceneNodeTintRecord 大小不匹配");
static_assert(sizeof(GxStagePresetRecord) == 0x30,
              "GxStagePresetRecord 大小不匹配");
static_assert(sizeof(RenderOverridePresetChannelDataRecord) == 0x28,
              "RenderOverridePresetChannelDataRecord 大小不匹配");
static_assert(sizeof(RenderOverridePresetChannelResolverRecord) == 0x54,
              "RenderOverridePresetChannelResolverRecord 大小不匹配");
static_assert(sizeof(RenderOverrideTransformAuxRecord) == 0x50,
              "RenderOverrideTransformAuxRecord 大小不匹配");
static_assert(sizeof(RenderOverrideDependencyGateOutputRecord) == 0x10,
              "RenderOverrideDependencyGateOutputRecord 大小不匹配");
static_assert(sizeof(RenderOverrideCompactScalarOutputEntry) == 0x04,
              "RenderOverrideCompactScalarOutputEntry 大小不匹配");
static_assert(sizeof(CModelPartFrameWindowRecord) == 0x10,
              "CModelPartFrameWindowRecord 大小不匹配");
static_assert(sizeof(CModelPartSequenceFrameRecord) == 0x8C,
              "CModelPartSequenceFrameRecord 大小不匹配");
static_assert(sizeof(CModelPartStateDefRecord) == 0xE4,
              "CModelPartStateDefRecord 大小不匹配");
static_assert(sizeof(CModelPartWeightVisibilityRecord) == 0x38,
              "CModelPartWeightVisibilityRecord 大小不匹配");
static_assert(sizeof(CModelVisibilityDependencyRecord) == 0x1C,
              "CModelVisibilityDependencyRecord 大小不匹配");
static_assert(sizeof(ParticleEmitterOverrideSlot) == 0x80,
              "ParticleEmitterOverrideSlot 大小不匹配");
static_assert(sizeof(PlaneParticleEmitterOverrideSlot) == 0x8C,
              "PlaneParticleEmitterOverrideSlot 大小不匹配");
static_assert(sizeof(CGxuLight) == 0x2C, "CGxuLight 大小不匹配");
static_assert(sizeof(RenderOverrideLocalPointOutputRecord) == 0x40,
              "RenderOverrideLocalPointOutputRecord 大小不匹配");
static_assert(sizeof(CAnimRibbonObjStatus) == 0x74,
              "CAnimRibbonObjStatus 大小不匹配");
static_assert(sizeof(CParticleEmitterRuntimeParticle) == 0x28,
              "CParticleEmitterRuntimeParticle 大小不匹配");
static_assert(sizeof(CParticleEmitterRuntime) == 0x68,
              "CParticleEmitterRuntime 大小不匹配");
static_assert(sizeof(CAnimRibbonObj) == 0x16C,
              "CAnimRibbonObj 大小不匹配");
static_assert(sizeof(MeshLayerStateRecord) == 0x24,
              "MeshLayerStateRecord 大小不匹配");
static_assert(sizeof(MeshLayerDispatchRecord) == 0x2C,
              "MeshLayerDispatchRecord 大小不匹配");
static_assert(sizeof(MeshAuxResourceEntry) == 0x2C,
              "MeshAuxResourceEntry 大小不匹配");
static_assert(sizeof(LayerInfo) == 0x14, "LayerInfo 大小不匹配");
static_assert(sizeof(MeshData) == 0x128, "MeshData 大小不匹配");

// ============================================================================
// MemHack中的数组模板
// ============================================================================

template <typename T>
struct GameArray {
  uint8_t             ukn_0x0[0x4];
  uint32_t            capacity;       // 0x4
  T*                  arr;            // 0x8
  uint32_t            size;           // 0xC
}; // sizeof = 0x10

template <typename T>
struct GameArray2 {
  uint32_t            capacity;       // 0x0
  uint32_t            size;           // 0x8
  T*                  arr;            // 0x4
  uint8_t             ukn_0xC[0x4];
}; // sizeof = 0x10

template <typename T>
struct GameSimpleArray {
  uint32_t            capacity;       // 0x0
  uint32_t            size;           // 0x4
  T*                  arr;            // 0x8
}; // sizeof = 0xC

template <typename T>
struct GameStack {
  uint32_t            capacity;       // 0x0
  uint32_t            top;            // 0x4
  T*                  arr;            // 0x8
};

// ============================================================================
// 哈希相关结构
// ============================================================================

struct HashCargo {
  HashGroup           hash;           // 0x0
  uint8_t             ukn_0x8[0x4];  // = 0
};

// ============================================================================
// 引用计数字符串
// ============================================================================

struct RCString {
  void**              vf_table;       // 0x0
  uint32_t            ref_count;     // 0x4
  const CStringRep*    string_rep;    // 0x8
  
  const char*         get_str() const { return string_rep ? string_rep->get_str() : nullptr; }
  bool                set_str(const char* str);
  
  RCString(const CStringRep& rep);
  RCString();
}; // sizeof = 0xC

} // namespace native
} // namespace war3
