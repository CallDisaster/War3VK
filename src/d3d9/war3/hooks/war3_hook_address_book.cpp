#include "war3_hook_address_book.h"

namespace dxvk::war3::hooks {

const War3HookAddressBook &GetWar3HookAddressBook127a() {
  static constexpr War3HookAddressBook kBook = {
      // Bootstrap / 生命周期 / JASS
      0x1E9A50, // initJassNatives
      0x1BFAC0, // executeJassFunction
      0x7F2B40, // executeJassFunctionInternal（函数入口，修正中段地址导致的深层 Hook 崩溃）
      0x7F1A20, // jassInterpreterMainLoop
      0x7EF590, // executeNativeFunction
      0x1E2500, // nativeGetFloatGameState（直接实现入口；也会从注册链按名字校准）
      0x7F1810, // jassFuncPauseAndCreateFrame
      0x7E2FE0, // getTlsJassData
      0x7ECE50, // regFuncAddr2Handle
      0x7EDA90, // computeHandleMemoryAddr
      0x890960, // mainRunner
      0x1EF710, // mainRunnerAlt
      0x05F710, // mainLoopRoot
      0x059890, // eventMainCallback
      0x059B00, // eventMessagePump
      0x05A310, // eventDispatch
      0x059D70, // dispatchCase0Fn
      0x059DC0, // dispatchCase1Fn
      0x05A2A0, // dispatchCase2Fn
      0x059E40, // dispatchCase3Fn
      0x05A270, // dispatchCase4Fn
      0x059890, // dispatchCase5GateFn
      0x059E00, // dispatchCase5CommitFn
      0x059E10, // dispatchCase6Fn
      0x059E90, // dispatchCase7Fn
      0x059F00, // dispatchCase8Fn
      0x059F70, // dispatchCase9Fn
      0x05A060, // dispatchCase10Fn
      0x05A1F0, // dispatchCase11Fn
      0x05A0E0, // dispatchCase12Fn
      0x05A160, // dispatchCase13Fn
      0x059890, // dispatchCase14GateFn
      0x057470, // engineTlsPump
      0x05DE80, // engineSelectWorker
      0x0603B0, // engineRunCallbacks
      0x05B080, // engineQueueFlush
      0x05FB10, // engineFinalizeTick
      0x05EE90, // engineReschedule
      0x05DEE0, // enginePrepareWait
      0x158940, // engineWaitGate
      0x05FCA0, // enginePrepareDispatch
      0x05FD10, // engineFinalizeDispatch
      0x05FC10, // engineTickUpdate
      0x05DCE0, // engineFinalizeWorker
      0x060500, // engineComputeWakeDelta
      0x1648A0, // engineSleepGate
      0x164B00, // engineSleepGateInner
      0x354240, // gamePause
      0x0EC6B0, // getD3d9Parameters
      0x151F90, // windowMessageTargetLookup
      0xBDAA1C, // windowSizeLParamState
      0x139800, // flushAndReset
      0x0CAA90, // uiDispatch
      0x184F00, // uiRenderableRender

      // Render 域
      0x7B28F0, // renderDispatcher
      0x368480, // worldFrameUpdateAndPreparePasses
      0x3681C0, // worldRenderScene
      0x139190, // renderQueueAddBatch
      0x1375C0, // renderBatchSubmit
      0x137AF0, // aucTransparentAddEntry
      0x7B7830, // sceneSubmitBatch
      0x0CB110, // worldObjectListEntryWrite
      0x184EE0, // worldObjectEntryRender
      0x363020, // worldDispatch
      0x368E30, // worldObjectsRenderGroup
      0x13A5E0, // dispatchCommon
      0x13A780, // dispatchSpecial
      0x138F70, // applyDrawStateAndDraw (ApplyDrawStateAndSamplerPair -> Gx draw)
      0x1380A0, // flushSortedItems
      0x737110, // terrainRenderAllTiles

      // Shadow 域
      0x737620, // terrainShadowLayer
      0x737500, // terrainShadowListA
      0x737400, // terrainShadowListB
      0x73F7A0, // shadowUpdateWriteEntry
      0x76D790, // shadowProjectorAddSimple
      0x76D800, // shadowProjectorAddFromObject
      0x713250, // shadowRegisterImageEntry
      0x74DB30, // shadowToggleStaticStampFromObject
      0x74DE40, // shadowToggleEmitterStamp (函数入口，74DF50 为内部 call 点)
      0x74E420, // shadowPathStaticStampToggle
      0x764AC0, // shadowProjectorSimpleBridge
      0x38D7A0, // shadowPathObjectProjectorRuntime
      0x1DEEA0, // shadowPathObjectProjectorJassBridge
      0x7291DC, // shadowRegisterRetWithParams
      0x74DAB6, // shadowRegisterRetSelectionCircle
      0x74DBFA, // shadowRegisterRetStaticStamp
      0x74DF55, // shadowRegisterRetEmitterStamp
      0x76D44A, // shadowRegisterRetObjectBridge
      0x76D5A4, // shadowRegisterRetMarkOcclusion
      0x76D69A, // shadowRegisterRetFromPoint
      0x76D719, // shadowRegisterRetFromTwoPoints
      0x65A140, // widgetRegisterFootprintAndShadowMask（论文第6章中央 sync 入口）
      0x234710, // terrainShadowWriteMaskRegion（论文 §6.7.1 方案 A）

      // RenderQueue 数据区
      0xBC6BAC, // rqNumOfElements
      0xBC6BB0, // rqBatchArrayPtr
      0xBC6BBC, // rqNumOfTransparent (AUCTransparent_Count)
      0xBC6BC0, // rqTransparentArrayBasePtr (AUCTransparent_Array)
      0xBD0828, // rqTransparentSortedPtrs (AUCTransparent_SortedPtrs)
      0xBC6BA0, // rqSortedBatchCount
      0xBC6BE8, // rqSortedBatchPtrs
      0xBDA4D0, // rqStateOptEnabled
      0xBDA4D4, // rqStateCleanupPending
      0xBE40A8, // handleManager
      0xBE4238, // gameWar3

      // RenderQueue/设备辅助函数
      0x1378B0, // rqItemComparator
      0x0E34B0, // gxApplyStateBlock
      0x13A9B0, // rqStageUpdate
      0x0E3640, // gxCleanup74
      0x0E3670, // gxCleanup78
      0x138210, // rqFlushTransparent
      0x13A0E0, // rqTransparentDispatchType0
      0x198C00, // rqTransparentDispatchType1
      0x19DFF0, // rqTransparentDispatchType2
      0x19BC20, // rqTransparentDispatchType3
      0x13A0B0, // rqTransparentDispatchType4
  };
  return kBook;
}

} // namespace dxvk::war3::hooks
