#pragma once

#include <cstdint>

namespace dxvk::war3::hooks {

/**
 * @brief War3 1.27a Hook RVA 地址清单。
 *
 * 说明：
 * - 所有字段均为相对 `Game.dll` 基址的 RVA；
 * - 该结构用于统一管理 Hook/数据偏移，避免多模块散落硬编码；
 * - 当前版本目标：`1.27.x`。
 */
struct War3HookAddressBook {
  // -------------------------------------------------------------------------
  // Bootstrap / 生命周期 / JASS
  // -------------------------------------------------------------------------
  uintptr_t initJassNatives = 0;
  uintptr_t executeJassFunction = 0;
  uintptr_t executeJassFunctionInternal = 0;
  uintptr_t jassInterpreterMainLoop = 0;
  uintptr_t executeNativeFunction = 0;
  uintptr_t nativeGetFloatGameState = 0;
  uintptr_t jassFuncPauseAndCreateFrame = 0;
  uintptr_t getTlsJassData = 0;
  uintptr_t regFuncAddr2Handle = 0;
  uintptr_t computeHandleMemoryAddr = 0;
  uintptr_t mainRunner = 0;
  uintptr_t mainRunnerAlt = 0;
  uintptr_t mainLoopRoot = 0;
  uintptr_t eventMainCallback = 0;
  uintptr_t eventMessagePump = 0;
  uintptr_t eventDispatch = 0;
  // EventDispatch case0~14 对应子函数入口（用于函数级归因/后续可选 Hook）
  uintptr_t dispatchCase0Fn = 0;
  uintptr_t dispatchCase1Fn = 0;
  uintptr_t dispatchCase2Fn = 0;
  uintptr_t dispatchCase3Fn = 0;
  uintptr_t dispatchCase4Fn = 0;
  uintptr_t dispatchCase5GateFn = 0;
  uintptr_t dispatchCase5CommitFn = 0;
  uintptr_t dispatchCase6Fn = 0;
  uintptr_t dispatchCase7Fn = 0;
  uintptr_t dispatchCase8Fn = 0;
  uintptr_t dispatchCase9Fn = 0;
  uintptr_t dispatchCase10Fn = 0;
  uintptr_t dispatchCase11Fn = 0;
  uintptr_t dispatchCase12Fn = 0;
  uintptr_t dispatchCase13Fn = 0;
  uintptr_t dispatchCase14GateFn = 0;
  uintptr_t engineTlsPump = 0;
  uintptr_t engineSelectWorker = 0;
  uintptr_t engineRunCallbacks = 0;
  uintptr_t engineQueueFlush = 0;
  uintptr_t engineFinalizeTick = 0;
  uintptr_t engineReschedule = 0;
  uintptr_t enginePrepareWait = 0;
  uintptr_t engineWaitGate = 0;
  uintptr_t enginePrepareDispatch = 0;
  uintptr_t engineFinalizeDispatch = 0;
  uintptr_t engineTickUpdate = 0;
  uintptr_t engineFinalizeWorker = 0;
  uintptr_t engineComputeWakeDelta = 0;
  uintptr_t engineSleepGate = 0;
  uintptr_t engineSleepGateInner = 0;
  uintptr_t gamePause = 0;
  uintptr_t getD3d9Parameters = 0;
  uintptr_t windowMessageTargetLookup = 0;
  uintptr_t windowSizeLParamState = 0;
  uintptr_t flushAndReset = 0;
  uintptr_t uiDispatch = 0;
  uintptr_t uiRenderableRender = 0;

  // -------------------------------------------------------------------------
  // Render 域
  // -------------------------------------------------------------------------
  uintptr_t renderDispatcher = 0;
  uintptr_t worldFrameUpdateAndPreparePasses = 0;
  uintptr_t worldRenderScene = 0;
  uintptr_t renderQueueAddBatch = 0;
  uintptr_t renderBatchSubmit = 0;
  uintptr_t aucTransparentAddEntry = 0;
  uintptr_t sceneSubmitBatch = 0;
  uintptr_t worldObjectListEntryWrite = 0;
  uintptr_t worldObjectEntryRender = 0;
  uintptr_t worldDispatch = 0;
  uintptr_t worldObjectsRenderGroup = 0;
  uintptr_t dispatchCommon = 0;
  uintptr_t dispatchSpecial = 0;
  uintptr_t applyDrawStateAndDraw = 0;
  uintptr_t flushSortedItems = 0;
  uintptr_t terrainRenderAllTiles = 0;

  // -------------------------------------------------------------------------
  // Shadow 域
  // -------------------------------------------------------------------------
  uintptr_t terrainShadowLayer = 0;
  uintptr_t terrainShadowListA = 0;
  uintptr_t terrainShadowListB = 0;
  uintptr_t shadowUpdateWriteEntry = 0;
  uintptr_t shadowProjectorAddSimple = 0;
  uintptr_t shadowProjectorAddFromObject = 0;
  uintptr_t shadowRegisterImageEntry = 0;
  uintptr_t shadowToggleStaticStampFromObject = 0;
  uintptr_t shadowToggleEmitterStamp = 0;
  uintptr_t shadowPathStaticStampToggle = 0;
  uintptr_t shadowProjectorSimpleBridge = 0;
  uintptr_t shadowPathObjectProjectorRuntime = 0;
  uintptr_t shadowPathObjectProjectorJassBridge = 0;
  // RegisterImage 关键调用点“返回地址”（RVA），用于精确来源识别。
  uintptr_t shadowRegisterRetWithParams = 0;      // 0x7291DC
  uintptr_t shadowRegisterRetSelectionCircle = 0; // 0x74DAB6
  uintptr_t shadowRegisterRetStaticStamp = 0;     // 0x74DBFA
  uintptr_t shadowRegisterRetEmitterStamp = 0;    // 0x74DF55
  uintptr_t shadowRegisterRetObjectBridge = 0;    // 0x76D44A
  uintptr_t shadowRegisterRetMarkOcclusion = 0;   // 0x76D5A4
  uintptr_t shadowRegisterRetFromPoint = 0;       // 0x76D69A
  uintptr_t shadowRegisterRetFromTwoPoints = 0;   // 0x76D719

  // -------------------------------------------------------------------------
  // RenderQueue 数据区
  // -------------------------------------------------------------------------
  uintptr_t rqNumOfElements = 0;
  uintptr_t rqBatchArrayPtr = 0;
  uintptr_t rqNumOfTransparent = 0;
  uintptr_t rqTransparentArrayBasePtr = 0;
  uintptr_t rqTransparentSortedPtrs = 0;
  uintptr_t rqSortedBatchCount = 0;
  uintptr_t rqSortedBatchPtrs = 0;
  uintptr_t rqStateOptEnabled = 0;
  uintptr_t rqStateCleanupPending = 0;
  uintptr_t handleManager = 0;
  uintptr_t gameWar3 = 0;

  // -------------------------------------------------------------------------
  // RenderQueue/设备辅助函数
  // -------------------------------------------------------------------------
  uintptr_t rqItemComparator = 0;
  uintptr_t gxApplyStateBlock = 0;
  uintptr_t rqStageUpdate = 0;
  uintptr_t gxCleanup74 = 0;
  uintptr_t gxCleanup78 = 0;
  uintptr_t rqFlushTransparent = 0;
  uintptr_t rqTransparentDispatchType0 = 0;
  uintptr_t rqTransparentDispatchType1 = 0;
  uintptr_t rqTransparentDispatchType2 = 0;
  uintptr_t rqTransparentDispatchType3 = 0;
  uintptr_t rqTransparentDispatchType4 = 0;
};

/**
 * @brief 获取 War3 1.27a 地址清单。
 * @return 常量地址清单引用。
 */
const War3HookAddressBook &GetWar3HookAddressBook127a();

} // namespace dxvk::war3::hooks
