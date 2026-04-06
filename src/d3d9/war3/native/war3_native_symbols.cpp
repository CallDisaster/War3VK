#include "war3_native_hooks.h"
#include <cstdio>

namespace war3 {
namespace native {
namespace global {

// ============================================================================
// Render Queue Globals
// ============================================================================
uint32_t g_RenderQueue_BatchCapacity = 0;       // @ 0xBC6BA8
uint32_t g_RenderQueue_NumOfElements = 0;       // @ 0xBC6BAC
void *g_RenderQueue_BatchArray = nullptr;       // @ 0xBC6BB0
uint32_t g_RenderQueue_SortedCount = 0;         // @ 0xBC6BA0
uint32_t g_RenderQueue_BatchGrowStep = 0;       // @ 0xBC6BB4
void **g_RenderQueue_SortedPtrs = nullptr;      // @ 0xBC6BE8
uint32_t g_RenderQueue_StateOptEnabled = 0;     // @ 0xBDA4D0
uint32_t g_RenderQueue_StateCleanupPending = 0; // @ 0xBDA4D4
uint8_t *g_RenderQueue_StageInitialized = nullptr; // @ 0xBDA4D8
uint32_t g_RenderQueue_StageCount = 0;             // @ 0xBDA4E0
uint32_t g_RenderQueue_StageCountInit = 0;         // @ 0xBDA4E4

// ============================================================================
// Transparent Queue Globals
// ============================================================================
uint32_t g_AUCTransparent_Capacity = 0;       // @ 0xBC6BB8
uint32_t g_AUCTransparent_Count = 0;          // @ 0xBC6BBC
void *g_AUCTransparent_Array = nullptr;       // @ 0xBC6BC0
uint32_t g_AUCTransparent_GrowStep = 0;       // @ 0xBC6BC4
uint32_t g_AUCTransparent_SortedCount = 0;    // @ 0xBC6BA4
void **g_AUCTransparent_SortedPtrs = nullptr; // @ 0xBD0828

// ============================================================================
// Camera Globals
// ============================================================================
float g_RenderCamera_PosXY[2] = {0.0f, 0.0f}; // Camera XY
float g_RenderCamera_PosZ = 0.0f;             // Camera Z

// ============================================================================
// TeamColor Globals
// ============================================================================
uint32_t g_TeamColorCount = 24;       // @ 0xBE6184
void **g_TeamColorTextures = nullptr; // @ 0xBE6188
uint32_t g_TeamColorFriend = 0;       // @ 0xBE62F0
uint32_t g_TeamColorEnemy = 0;        // @ 0xBE62FC

} // namespace global

extern "C" {

void RenderQueue_FlushAndReset(void) {
  if (original::RenderQueue_FlushAndReset)
    original::RenderQueue_FlushAndReset();
}

void Terrain_RenderStage(int terrainType) {
  if (original::Terrain_RenderStage)
    original::Terrain_RenderStage(terrainType, 0);
}

void CWorld_SetShadowMode(int handle, int enabled) {
  if (original::CWorld_SetShadowMode)
    original::CWorld_SetShadowMode(handle, enabled);
}

void CWorld_ToggleGroup1ShadowPass(CWorldFrameWar3 *world, int enabled) {
  if (original::CWorld_ToggleGroup1ShadowPass)
    original::CWorld_ToggleGroup1ShadowPass(world, enabled);
}

void WAR3_NATIVE_CB RenderCategory_Enable(CWorldFrameWar3 *world, int reserved,
                                          RenderCategoryMask mask,
                                          RenderCategoryMask current) {
  if (original::RenderCategory_Enable)
    original::RenderCategory_Enable(world, reserved, (int)mask, (int)current);
}

void WAR3_NATIVE_CB RenderCategory_Disable(CWorldFrameWar3 *world, int reserved,
                                           RenderCategoryMask mask,
                                           RenderCategoryMask current) {
  if (original::RenderCategory_Disable)
    original::RenderCategory_Disable(world, reserved, (int)mask, (int)current);
}

void WAR3_NATIVE_CB ApplyCategoryMode(CWorldFrameWar3 *world, int reserved,
                                      int mode, int flag) {
  if (original::ApplyCategoryMode)
    original::ApplyCategoryMode(world, reserved, mode, flag);
}

void StateCleanup(void *cleanupPtr) {
  if (original::StateCleanup)
    original::StateCleanup(cleanupPtr, 0);
}

bool CWorld_HasPostProcessPreview() {
  if (original::CWorld_HasPostProcessPreview_Func)
    return original::CWorld_HasPostProcessPreview_Func() != 0;
  return false;
}

void CWorld_CommitPostProcessQueue() {
  if (original::CWorld_CommitPostProcessQueue_Func)
    original::CWorld_CommitPostProcessQueue_Func();
}

void CWorld_RenderPreviewContext(void *context) {
  if (original::CWorld_RenderPreviewContext_Func)
    original::CWorld_RenderPreviewContext_Func(context, 0);
}

void RenderStage0_PreRenderContext(void *context) {
  if (original::RenderStage0_PreRenderContext_Func)
    original::RenderStage0_PreRenderContext_Func(context, 0);
}

void DebugRender_CheckFlags(uint32_t flags) {
  typedef void (*FuncType)(uint32_t);
  if (original::CWorld_RenderScene) {
    auto base = (uint8_t *)original::CWorld_RenderScene - 0x3681C0;
    ((FuncType)(base + 0x368A90))(flags);
  }
}

int CWorld_RenderSelectionManagerStage() {
  if (original::CWorld_RenderSelectionManagerStage_Func)
    return original::CWorld_RenderSelectionManagerStage_Func();
  return 0;
}

void CWorld_RenderStage21IndicatorTail(void *context) {
  if (original::CWorld_RenderStage21IndicatorTail_Func)
    original::CWorld_RenderStage21IndicatorTail_Func(context, 0);
}

void RenderSceneCleanupContext_Flush(void *context) {
  if (original::RenderSceneCleanupContext_Flush_Func)
    original::RenderSceneCleanupContext_Flush_Func(context, 0);
}

int Visibility_Check(void *context, int index) {
  typedef int (*FuncType)(void *, int);
  if (original::CWorld_RenderScene) {
    auto base = (uint8_t *)original::CWorld_RenderScene - 0x3681C0;
    return ((FuncType)(base + 0x777FE0))(context, index);
  }
  return 0;
}

void SceneNode_AddTransparentList0(SceneNode *node, void *world) {}
void SceneNode_AddTransparentList2(SceneNode *node, void *world) {}
void SceneNode_AddTransparentList3(SceneNode *node, void *world) {}
void SceneNode_AddTransparentList4(SceneNode *node, void *world) {}

void *List_GetData(ListHeader *list) { return list ? list->data : nullptr; }
uint32_t List_GetCount(ListHeader *list) { return list ? list->count : 0; }

} // extern "C"

} // namespace native
} // namespace war3
