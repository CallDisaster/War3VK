#pragma once

#include "war3_native_renderer.h"

namespace dxvk {
class D3D9DeviceEx;
}

namespace war3 {
namespace native {

// Hook 控制函数
extern "C" int InstallNativeRendererHooks(void *baseAddress);
extern "C" int UninstallNativeRendererHooks();
void InitializeNativeRendererHooks(dxvk::D3D9DeviceEx *device);

// 原始函数指针访问 (用于逐步还原校验)
namespace original {
typedef int(WAR3_NATIVE_CB *CWorld_RenderScene_t)(CWorldFrameWar3 *, int);
typedef int(WAR3_NATIVE_CB *RenderWorld_DispatchStage_t)(CWorldFrameWar3 *, int, int,
                                                         int, int, int);
typedef int(__cdecl *WorldObjects_RenderGroup_t)(int *, int, int);
typedef int(WAR3_NATIVE_CB *WorldObjectEntry_Render_t)(int, int);
typedef void(WAR3_NATIVE_CB *RenderQueue_AddBatch_t)(int, int);
typedef void(WAR3_NATIVE_CB *RenderBatch_Submit_t)(SceneNode *, int);
typedef unsigned int(__cdecl *RenderQueue_FlushSortedItems_t)();
typedef void(__cdecl *RenderQueue_FlushAndReset_t)();
typedef int(__cdecl *RenderQueue_ItemComparator_t)(const void *, const void *);
typedef bool(WAR3_NATIVE_CB *RenderQueue_ItemLess_t)(
    const RenderBatchElement *, int, const RenderBatchElement *);
typedef int(WAR3_NATIVE_CB *RenderQueue_Dispatch_Common_t)(void *, int, int,
                                                           int);
typedef int(WAR3_NATIVE_CB *RenderQueue_Dispatch_Special_t)(void *, int, int);
typedef bool(WAR3_NATIVE_CB *RenderBatch_CanEnqueueToMainQueue_t)(SceneNode *,
                                                                  int, void *);
typedef void(__cdecl *AUCTransparent_AddEntry_t)(void *, uint32_t,
                                                 const float *, uint32_t);

// 实用工具函数
typedef void(WAR3_NATIVE_CB *StateCleanup_t)(void *, int);
typedef void(WAR3_NATIVE_CB *RenderCategory_Enable_t)(CWorldFrameWar3 *, int, int,
                                                      int);
typedef void(WAR3_NATIVE_CB *RenderCategory_Disable_t)(CWorldFrameWar3 *, int, int,
                                                       int);
typedef void(WAR3_NATIVE_CB *ApplyCategoryMode_t)(CWorldFrameWar3 *, int, int,
                                                  int);
typedef int(WAR3_NATIVE_CB *CWorld_SetShadowMode_t)(int, int);
typedef void(WAR3_NATIVE_CB *CWorld_ToggleGroup1ShadowPass_t)(CWorldFrameWar3 *,
                                                              int);
typedef void(WAR3_NATIVE_CB *Terrain_RenderStage_t)(int, int);
typedef void(WAR3_NATIVE_CB *RenderSceneCleanupContext_Flush_t)(void *, int);
typedef void(WAR3_NATIVE_CB *RenderStage0_PreRenderContext_t)(void *, int);
typedef int(__cdecl *CWorld_HasPostProcessPreview_t)();
typedef int(__cdecl *CWorld_CommitPostProcessQueue_t)();
typedef int(WAR3_NATIVE_CB *CWorld_RenderPreviewContext_t)(void *, int);
typedef int(__cdecl *CWorld_RenderSelectionManagerStage_t)();
typedef int(WAR3_NATIVE_CB *CWorld_RenderStage21IndicatorTail_t)(void *, int);

extern CWorld_RenderScene_t CWorld_RenderScene;
extern RenderWorld_DispatchStage_t RenderWorld_DispatchStage;
extern WorldObjects_RenderGroup_t WorldObjects_RenderGroup;
extern WorldObjectEntry_Render_t WorldObjectEntry_Render;
extern RenderQueue_AddBatch_t RenderQueue_AddBatch;
extern RenderBatch_Submit_t RenderBatch_Submit;
extern RenderQueue_FlushSortedItems_t RenderQueue_FlushSortedItems;
extern RenderQueue_FlushAndReset_t RenderQueue_FlushAndReset;
extern RenderQueue_ItemComparator_t RenderQueue_ItemComparator;
extern RenderQueue_ItemLess_t RenderQueue_ItemLess;
extern RenderQueue_Dispatch_Common_t RenderQueue_Dispatch_Common;
extern RenderQueue_Dispatch_Special_t RenderQueue_Dispatch_Special;
extern RenderBatch_CanEnqueueToMainQueue_t RenderBatch_CanEnqueueToMainQueue;
extern AUCTransparent_AddEntry_t AUCTransparent_AddEntry;

extern StateCleanup_t StateCleanup;
extern RenderCategory_Enable_t RenderCategory_Enable;
extern RenderCategory_Disable_t RenderCategory_Disable;
extern ApplyCategoryMode_t ApplyCategoryMode;
extern CWorld_SetShadowMode_t CWorld_SetShadowMode;
extern CWorld_ToggleGroup1ShadowPass_t CWorld_ToggleGroup1ShadowPass;
extern Terrain_RenderStage_t Terrain_RenderStage;
extern RenderSceneCleanupContext_Flush_t RenderSceneCleanupContext_Flush_Func;
extern RenderStage0_PreRenderContext_t RenderStage0_PreRenderContext_Func;
extern CWorld_HasPostProcessPreview_t CWorld_HasPostProcessPreview_Func;
extern CWorld_CommitPostProcessQueue_t CWorld_CommitPostProcessQueue_Func;
extern CWorld_RenderPreviewContext_t CWorld_RenderPreviewContext_Func;
extern CWorld_RenderSelectionManagerStage_t CWorld_RenderSelectionManagerStage_Func;
extern CWorld_RenderStage21IndicatorTail_t CWorld_RenderStage21IndicatorTail_Func;
} // namespace original

} // namespace native
} // namespace war3
