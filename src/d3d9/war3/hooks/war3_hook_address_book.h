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
  // Storm_EventLoop 在 WM_ACTIVATEAPP=后台时读取的空闲 Sleep 毫秒来源。
  uintptr_t backgroundIdleSleepMs = 0;
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
  uintptr_t applyDrawStateAndSamplerPair = 0;
  uintptr_t applyDrawStateAndDraw = 0;
  uintptr_t gxDeviceD3dDynamicVertexUpload = 0;
  uintptr_t flushSortedItems = 0;
  uintptr_t terrainRenderAllTiles = 0;

  // WorldFrameUpdateAndPreparePasses 内部第一层固定 callee。
  // 仅供 PERF_LEVEL=2 + DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS=1
  // 的显式诊断安装；默认运行时不会安装这些 detour。
  uintptr_t worldPrepareCameraBuildFrustum = 0;
  uintptr_t worldPrepareTerrainShadowFlush = 0;
  uintptr_t worldPrepareTerrainExtraPass = 0;
  uintptr_t worldPrepareShadowProjectorFlush = 0;
  uintptr_t worldPrepareTargetIndicatorRingAdvance = 0;
  uintptr_t worldPrepareCinematicFilterTimeAdvance = 0;
  uintptr_t worldPrepareRuntimeFlagClockAdvance3B8760 = 0;

  // WorldFrameUpdateAndPreparePasses 的剩余固定 callee。与上组分离是为了
  // 允许独立打开低污染残余分账；0x368E90 依赖调用方 EDI 隐式实参，
  // 不满足普通 MinHook C++ detour ABI，故刻意不进入地址簿。
  uintptr_t worldPrepareFlushDeferredSelectionObjects = 0;
  uintptr_t worldPrepareGlobalRenderCallbackPass = 0;
  uintptr_t worldPrepareRenderWaypointIndicators = 0;

  // WorldFrameUpdateAndPreparePasses 内尚未归属的核心阶段。调用约定已由
  // 1.27a 机器码的 call-site、callee prologue 与 RET N 三方确认；只在
  // PERF_LEVEL=2 + DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS=1 时安装。
  uintptr_t worldPrepareFrameUpdateGate = 0;
  uintptr_t worldPrepareGameUiFrameSync = 0;
  uintptr_t worldPrepareUpdateIndicatorAnchor = 0;
  uintptr_t worldPrepareCameraAdvance = 0;
  uintptr_t worldPrepareCameraPrepareConstants = 0;
  uintptr_t worldPrepareViewProjPrepare = 0;
  uintptr_t worldPrepareSceneQueryFlushSync = 0;
  uintptr_t worldPrepareFixedPointRemap = 0;
  uintptr_t worldPreparePostVisibilityGlobalAdvanceA = 0;
  // 0x378420 数组扫描命中后的唯一重型子阶段。0x6374A0 仅从 entry+0x50
  // 取出 this 并尾跳到该入口；直接量内层可把外层 self 收敛为扫描/句柄校验。
  uintptr_t worldPreparePostVisibilityFrameAnchorUpdate = 0;
  // FrameAnchorUpdate 内唯一的 579-byte 投影/可见性查询；fastcall ABI 为
  // ECX=world object、EDX=2-float output、栈上第三个 output 指针。
  uintptr_t worldPreparePostVisibilityFrameAnchorVisibilityQuery = 0;
  uintptr_t worldPreparePostVisibilityGlobalAdvanceB = 0;
  uintptr_t worldPrepareVisibilityTailAdvanceA = 0;
  uintptr_t worldPrepareVisibilityTailAdvanceB = 0;

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
  // UnitUI.slk unitShadow/buildingShadow 字段写入 CUnitUIManager type record。
  uintptr_t cunitUiRecordSetUnitShadow = 0;       // 0x3358C0 (+0x4C)
  uintptr_t cunitUiRecordSetStructureShadow = 0;  // 0x335A00
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

  // CWidget lifecycle 中央 sync 入口（30+ caller 都会调）。
  // 用于在 destructible/building/unit 创建/销毁/移动等任意 lifecycle 事件时
  // 抓取 widget 身份链（rawcode + jHandle），喂给 RenderObjectRegistry 兜底。
  uintptr_t widgetRegisterFootprintAndShadowMask = 0;  // 0x65A140

  // 2026-05-30：CDoodads 贴花阴影治理（魔兽自带可见静态阴影 + path blocker）。
  // 这两个 Toggle 函数是树木/装饰物/可破坏物/path blocker 的"地面贴花阴影"
  // 唯一对象级注册入口，分别写 RegisterImage(type=0) 与 RegisterImage(type=4)。
  // hook 入口在 mode>=1 时跳过 enable!=0 写入即可干净屏蔽，不影响 fog/LOS/path。
  uintptr_t terrainShadowToggleStaticStampFromObject = 0;  // 0x74DB30
  uintptr_t terrainShadowToggleEmitterStamp = 0;           // 0x74DE40

  // Phase 7.143 证伪归档：这两个 ListA 函数不是静态阴影消费点。
  // 实机验证表明 hook 后会干掉悬崖/地形 tile；IDA 复核显示
  // RenderAllEntries -> sub_6F725F80 按 148-byte stride 取地形 tile 几何。
  // 地址仅保留作历史诊断，生产默认禁止安装对应 hook。
  uintptr_t terrainShadowListARenderPreparedGroups = 0;  // 0x7370A0
  uintptr_t terrainShadowListARenderAllEntries = 0;      // 0x737110

  // Phase 7.116 旧实验地址：后续实测 dispatchToShapeEnterCount=0，
  // 默认不再作为静态阴影治理点。仅在显式诊断开关打开时用于灰度验证。
  uintptr_t terrainShadowDispatchToShape = 0;  // 0x234420

  // Phase 7.100/7.101 诊断地址：idx==3/maskIdx 方案已被实测推翻。
  // 该路径属于 fog/LOS/path/footprint 共享 mask grid，默认仅安装诊断 hook，
  // 不做 reject。静态阴影生产治理改走 RegisterImage/StaticStamp producer 端。
  uintptr_t terrainShadowWriteMaskRegion = 0;  // 0x234710

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
  uintptr_t gxDevice = 0;

  // CGxDeviceD3d object layout offsets. `gxDevice + 0x584` is the
  // IDirect3DDevice9* written by IDirect3D9::CreateDevice in 0x6F0EC400.
  uintptr_t gxDeviceD3dNativeDeviceOffset = 0;

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
