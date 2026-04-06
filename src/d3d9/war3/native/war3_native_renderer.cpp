// war3_native_renderer.cpp
// 核心逻辑接替实现
// 基于魔兽争霸III (1.27.x) 运行态逆向分析

#include "war3_native_renderer.h"
#include "../../d3d9_war3_debug.h"
#include "war3_native_hooks.h"


namespace war3 {
namespace native {

// ============================================================================
// 内部辅助逻辑
// ============================================================================

namespace {

bool HasShadowModeOrStage21ListBEntry(const CWorldFrameWar3 *world) {
  return world && world->shadowModeOrStage21ListBEntryIndex != -1;
}

void ResetRenderStateCaches(CWorldFrameWar3 *world) {
  world->currentRenderCategory = RenderCategoryMask::Invalid;
  world->currentCategoryMode = CategoryMode::Invalid;
}

void FinalizeCleanup(CWorldFrameWar3 *world) {
  if (world->currentRenderCategory != RenderCategoryMask::Invalid) {
    RenderCategory_Disable(world, 0, world->currentRenderCategory,
                           world->currentRenderCategory);
    world->currentRenderCategory = RenderCategoryMask::Invalid;
  }

  if (world->currentCategoryMode != CategoryMode::Invalid) {
    ApplyCategoryMode(world, 0, (int)world->currentCategoryMode, 0);
    world->currentCategoryMode = CategoryMode::Invalid;
  }
}

} // namespace

// ============================================================================
// Native 函数导出实现
// ============================================================================

#ifdef _MSC_VER
#define WAR3_NATIVE_CB __fastcall
#else
#define WAR3_NATIVE_CB __attribute__((fastcall))
#endif

extern "C" int WAR3_NATIVE_CB Native_CWorld_RenderScene(CWorldFrameWar3 *world,
                                                        int reserved) {
  if (!world)
    return 0;

  WAR3_RENDER_LOG("Native_CWorld_RenderScene ENTER world=%p\n", world);

  StateCleanup(world->dayNightTerrainRuntime);
  StateCleanup(world->dayNightUnitRuntime);
  if (world->stage0Context)
    StateCleanup(world->stage0Context);

  ResetRenderStateCaches(world);

  void *activeQueue = world->activeRenderQueue;
  if (HasShadowModeOrStage21ListBEntry(world))
    CWorld_SetShadowMode(world->shadowModeOrStage21ListBEntryIndex, 1);

  if (world->stage0GateEnabled && world->stage0Context &&
      world->stage0GateReady) {
    Native_RenderWorld_DispatchStage(
        world, 0, RenderStage::Stage0_PreRenderContext, CategoryMode::Default,
        RenderCategoryMask::Stage0, 0);
  }

  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage1_TerrainShadow0,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(
      world, 0, RenderStage::Stage13_Group2, CategoryMode::Standard,
      RenderCategoryMask::Opaque, (int)(uintptr_t)activeQueue);

  RenderQueue_FlushAndReset();

  Native_RenderWorld_DispatchStage(
      world, 0, RenderStage::Stage19_TerrainShadow14, CategoryMode::Standard,
      RenderCategoryMask::Opaque, (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage9_TerrainShadow6,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage2_TerrainShadow1,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage3_TerrainShadow2,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(
      world, 0, RenderStage::Stage8_TerrainShadow10, CategoryMode::Standard,
      RenderCategoryMask::Opaque, (int)(uintptr_t)activeQueue);
  if (world->stage17Enabled) {
    Native_RenderWorld_DispatchStage(
        world, 0, RenderStage::Stage17_TerrainShadow11,
        CategoryMode::Standard, RenderCategoryMask::Opaque,
        (int)(uintptr_t)activeQueue);
  }
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage14_TerrainShadow7,
                                   CategoryMode::Overlay,
                                   RenderCategoryMask::Overlay,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage5_TerrainShadow5,
                                   CategoryMode::Overlay,
                                   RenderCategoryMask::Overlay,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage10_TerrainShadow4,
                                   CategoryMode::Overlay,
                                   RenderCategoryMask::Overlay,
                                   (int)(uintptr_t)activeQueue);
  if (HasShadowModeOrStage21ListBEntry(world)) {
    CWorld_ToggleGroup1ShadowPass(world, 1);
    Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage12_Group1,
                                     CategoryMode::Overlay,
                                     RenderCategoryMask::Overlay,
                                     (int)(uintptr_t)activeQueue);
  }
  Native_RenderWorld_DispatchStage(
      world, 0, RenderStage::Stage11_TerrainShadow12_Group0,
      CategoryMode::Overlay, RenderCategoryMask::Overlay,
      (int)(uintptr_t)activeQueue);

  RenderQueue_FlushAndReset();

  if (HasShadowModeOrStage21ListBEntry(world))
    CWorld_ToggleGroup1ShadowPass(world, 0);

  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage4_TerrainShadow3,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage7_TerrainShadow9,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(world, 0, RenderStage::Stage6_TerrainShadow8,
                                   CategoryMode::Standard,
                                   RenderCategoryMask::Opaque,
                                   (int)(uintptr_t)activeQueue);
  Native_RenderWorld_DispatchStage(
      world, 0, RenderStage::Stage20_TerrainShadow15, CategoryMode::Overlay,
      RenderCategoryMask::Overlay, (int)(uintptr_t)activeQueue);

  if (!activeQueue) {
    Native_RenderWorld_DispatchStage(world, 0,
                                     RenderStage::Stage15_SelectionManager,
                                     CategoryMode::Invalid,
                                     RenderCategoryMask::Invalid, 0);
    Native_RenderWorld_DispatchStage(world, 0,
                                     RenderStage::Stage18_PostProcessTail,
                                     CategoryMode::Overlay,
                                     RenderCategoryMask::Overlay, 0);
    Native_RenderWorld_DispatchStage(
        world, 0, RenderStage::Stage21_TerrainShadow13_IndicatorTail,
        CategoryMode::Invalid, RenderCategoryMask::Invalid, 0);
  }

  FinalizeCleanup(world);
  RenderSceneCleanupContext_Flush(world->renderSceneCleanupContext);
  return 0;
}

extern "C" int WAR3_NATIVE_CB Native_RenderWorld_DispatchStage(
    CWorldFrameWar3 *world, int reserved, RenderStage stageId,
    CategoryMode renderMode, RenderCategoryMask categoryMask, int activeQueue) {
  WAR3_RENDER_LOG("Native_RenderWorld_DispatchStage: stage=%d mode=%d cat=%d "
                  "activeQueue=%d\n",
                  (int)stageId, (int)renderMode, (int)categoryMask,
                  activeQueue);

  if (!world)
    return 0;

  if (activeQueue)
    renderMode = CategoryMode::Mode3;

  if (categoryMask != world->currentRenderCategory) {
    if (world->currentRenderCategory != RenderCategoryMask::Invalid) {
      RenderCategory_Disable(world, 0, world->currentRenderCategory,
                             world->currentRenderCategory);
    }
    if (categoryMask != RenderCategoryMask::Invalid) {
      RenderCategory_Enable(world, 0, categoryMask, categoryMask);
    }
    world->currentRenderCategory = categoryMask;
  }

  if (renderMode != world->currentCategoryMode) {
    if (world->currentCategoryMode != CategoryMode::Invalid) {
      ApplyCategoryMode(world, 0, (int)world->currentCategoryMode, 0);
    }
    if (renderMode != CategoryMode::Invalid) {
      ApplyCategoryMode(world, 0, (int)renderMode, 1);
    }
    world->currentCategoryMode = renderMode;
  }

  switch (stageId) {
  case RenderStage::Stage0_PreRenderContext:
    RenderStage0_PreRenderContext(world->stage0Context);
    return 0;
  case RenderStage::Stage1_TerrainShadow0:
    Terrain_RenderStage(0);
    return 0;
  case RenderStage::Stage2_TerrainShadow1:
    Terrain_RenderStage(1);
    return 0;
  case RenderStage::Stage3_TerrainShadow2:
    Terrain_RenderStage(2);
    return 0;
  case RenderStage::Stage4_TerrainShadow3:
    Terrain_RenderStage(3);
    return 0;
  case RenderStage::Stage5_TerrainShadow5:
    Terrain_RenderStage(5);
    return 0;
  case RenderStage::Stage6_TerrainShadow8:
    Terrain_RenderStage(8);
    return 0;
  case RenderStage::Stage7_TerrainShadow9:
    Terrain_RenderStage(9);
    return 0;
  case RenderStage::Stage8_TerrainShadow10:
    Terrain_RenderStage(10);
    return 0;
  case RenderStage::Stage9_TerrainShadow6:
    Terrain_RenderStage(6);
    return 0;
  case RenderStage::Stage10_TerrainShadow4:
    Terrain_RenderStage(4);
    return 0;
  case RenderStage::Stage11_TerrainShadow12_Group0:
    Terrain_RenderStage(12);
    Native_WorldObjects_RenderGroup(world, 0, (int)renderMode,
                                    WorldGroupIndex::Group0);
    return 0;
  case RenderStage::Stage12_Group1:
    Native_WorldObjects_RenderGroup(world, 0, (int)renderMode,
                                    WorldGroupIndex::Group1);
    return 0;
  case RenderStage::Stage13_Group2:
    Native_WorldObjects_RenderGroup(world, 0, (int)renderMode,
                                    WorldGroupIndex::Group2);
    return 0;
  case RenderStage::Stage14_TerrainShadow7:
    Terrain_RenderStage(7);
    return 0;
  case RenderStage::Stage15_SelectionManager:
    return CWorld_RenderSelectionManagerStage();
  case RenderStage::Stage16_DebugOverlay:
    // TODO(CWorld专题): 这一段依赖 dword_6FB66E24 全局 bitmask 和
    // sub_6F368A90 / sub_6F369560 多条分支，本轮先保留到 IDA 注释层。
    return 0;
  case RenderStage::Stage17_TerrainShadow11:
    Terrain_RenderStage(11);
    return 0;
  case RenderStage::Stage18_PostProcessTail:
    if (world->stage18PreviewEnabled && CWorld_HasPostProcessPreview() &&
        world->stage18PreviewContext) {
      CWorld_RenderPreviewContext(world->stage18PreviewContext);
    }
    if (CWorld_HasPostProcessPreview())
      CWorld_CommitPostProcessQueue();
    return 0;
  case RenderStage::Stage19_TerrainShadow14:
    Terrain_RenderStage(14);
    return 0;
  case RenderStage::Stage20_TerrainShadow15:
    Terrain_RenderStage(15);
    return 0;
  case RenderStage::Stage21_TerrainShadow13_IndicatorTail:
    Terrain_RenderStage(13);
    if (world->shadowModeOrStage21ListBEntryIndex != -1 &&
        world->stage21IndicatorReady) {
      CWorld_RenderStage21IndicatorTail(reinterpret_cast<void *>(
          static_cast<uintptr_t>(world->shadowModeOrStage21ListBEntryIndex)));
    }
    // TODO(CWorld专题): 原版这里还会走 dword_6FBE4238 -> sub_6F1C3200 ->
    // sub_6F26C7F0 的全局 tail render context 提交。
    return 0;
  default:
    return (int)stageId;
  }
}

extern "C" int WAR3_NATIVE_CB
Native_WorldObjects_RenderGroup(CWorldFrameWar3 *world, int reserved,
                                int categoryMode, WorldGroupIndex groupIdx) {
  if (!world)
    return 0;

  ListHeader *list = nullptr;
  switch (groupIdx) {
  case WorldGroupIndex::Group0:
    list = world->worldGroup0;
    break;
  case WorldGroupIndex::Group1:
    list = world->worldGroup1;
    break;
  case WorldGroupIndex::Group2:
    list = world->worldGroup2;
    break;
  default:
    return (int)groupIdx;
  }

  void *listData = List_GetData(list);
  const uint32_t count = List_GetCount(list);
  if (!listData || count == 0)
    return 0;

  auto *entries = reinterpret_cast<WorldObjectListEntry *>(listData);
  int result = 0;
  for (uint32_t i = 0; i < count; i++) {
    result = Native_WorldObjectEntry_Render(
        (int)(uintptr_t)entries[i].objectEntry, 0);
  }

  return result;
}

extern "C" void WAR3_NATIVE_CB Native_RenderQueue_AddBatch(SceneNode *sceneNode,
                                                           int reserved,
                                                           int categoryMode) {
  if (!sceneNode)
    return;
  // TODO(CWorld专题): RenderQueue_AddBatch 前半桥已确认，这里保留给后续接管层。
}

// ============================================================================
// Native_WorldObjectEntry_Render 实现
// ============================================================================

/**
 * @brief 世界对象条目桥接：PreRender -> RenderQueue_AddBatch
 * @param entry WorldObjectEntry 指针（寄存器风格桥接包装）
 * @return 返回值
 *
 * 已确认逻辑（0x6F184EE0）：
 * 1. 读取 entry+0x20 的 sceneNode
 * 2. 若 sceneNode 非空，先调用对象虚表 +0x14（vtable[5]）预渲染
 * 3. 然后跳到 RenderQueue_AddBatch(sceneNode)
 *
 * 它本身并不直接提交 draw call，而是“对象预处理 + 入主渲染队列”的桥。
 * 注意：当前 native 参考实现还没有在这里把 worldObjectEntry / jHandle
 * 写进 TLS 直桥；后续若要优化 render->logic 识别成本，这里是最自然的接线点。
 */
extern "C" int WAR3_NATIVE_CB Native_WorldObjectEntry_Render(int entry,
                                                             int reserved) {
  if (entry == 0) {
    return 0;
  }

  auto *entryPtr = reinterpret_cast<WorldObjectEntry *>(uintptr_t(entry));

  if (entryPtr->sceneNode) {
    // 调用 PreRender (vtable[5])
    void **vtable = reinterpret_cast<void **>(entryPtr->vtable);
    if (vtable) {
      typedef void(__thiscall * PreRenderFunc)(void *);
      PreRenderFunc preRender = (PreRenderFunc)vtable[5];
      if (preRender) {
        preRender((void *)(uintptr_t)entry);
      }
    }

    original::RenderQueue_AddBatch((int)(uintptr_t)entryPtr->sceneNode, 0);
    return 0;
  }

  return 0;
}

} // namespace native
} // namespace war3
