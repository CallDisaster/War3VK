// war3_internal_test_config.h - 内部测试开关（编译期）
//
// 说明：
// - 由于部分启动器/注入环境下无法可靠设置进程环境变量，
//   这里提供“直接编译进去”的测试开关，避免依赖 DXVK_WAR3_* 环境变量。
// - 这是内部开发用文件：需要上线/发布时可删除或改回环境变量控制。

#pragma once

#include <cstdint>

namespace dxvk::war3::internal {

// ========================================================================
// MainLoop 逻辑层覆盖分析模式（第四轮专项）
// ========================================================================
// 目的：
// 1) 最大化拆分 MainLoop/Wait/JASS 路径，压缩逻辑层未追踪项；
// 2) 在不改行为语义的前提下，优先提升“可观测性”而非帧率。
//
// 说明：
// - 本开关为“分析模式主开关”，会联动开启多项高频追踪；
// - 该模式会带来额外开销，仅建议在专项分析阶段使用。
inline constexpr bool kNativeMainLoopCoverageAnalysisMode = false;

// MainLoop 细粒度拆分开关（默认关闭，性能档按粗粒度统计运行）。
inline constexpr bool kNativeMainLoopDispatchBreakdownEnabled = false;
inline constexpr bool kNativeMainLoopFunctionBreakdownEnabled = false;
inline constexpr bool kNativeMainLoopRunCallbacksCallerBreakdownEnabled = false;
inline constexpr bool kNativeMainLoopTickUpdateSubBreakdownEnabled = false;

// ========================================================================
// Native Renderer Probe（对照采样/统计）
// ========================================================================
// 是否强制启用 NativeRendererProbe（无需 DXVK_WAR3_NATIVE_RENDERER）
inline constexpr bool kNativeRendererProbeEnabled = false;

// Probe 汇报间隔（帧数），越小日志越频繁（无需
// DXVK_WAR3_NATIVE_RENDERER_REPORT_INTERVAL）
inline constexpr uint32_t kNativeRendererProbeReportIntervalFrames = 60;

// ========================================================================
// RenderQueue A/B（排序替换）
// ========================================================================
// 是否启用 std::sort 替换 FlushSortedItems 内部 qsort（无需
// DXVK_WAR3_NATIVE_QUEUE_SORT）
// 注意：该路径仍属实验性，若出现战役/切图崩溃请先关闭再定位。
inline constexpr bool kNativeQueueSortEnabled = false;

/**
 * @brief 是否完全接管渲染队列的刷新逻辑 (Takeover)
 * 目标: 消除每帧 37.5 万次 Hook 桥接开销，冲击 170 FPS。
 */
inline constexpr bool kNativeQueueTakeoverEnabled =
    false; // 稳定优先：默认关闭全量接管，避免极端场景触发兼容性风险。

// 全量接管最小 Opaque 门槛（小于该值时回退原生 FlushSortedItems）。
// 说明：
// - 近期接管链路稳定后，下调门槛以提高 takeover 命中率，减少回落原生路径开销。
inline constexpr uint32_t kNativeQueueTakeoverFullMinOpaque = 8;
// 当存在透明队列时的最小 Opaque 门槛。
// 仍保留“透明场景更保守”策略，但不再过高，平衡稳定性与性能收益。
inline constexpr uint32_t kNativeQueueTakeoverFullMinOpaqueWhenTransparent = 24;

// 全量接管的“透明规模分层”阈值：
// - 当 transparentCount 超过该阈值时，默认回退原生 FlushSortedItems；
// - 目的：在高透明压力场景下优先稳态表现，避免全量接管在透明链路上出现负优化。
inline constexpr uint32_t kNativeQueueTakeoverFullMaxTransparent = 8192;

// 高 Opaque 压力下允许更高透明阈值，避免大规模主场景过早回退。
inline constexpr uint32_t kNativeQueueTakeoverFullHighOpaqueThreshold = 128;
inline constexpr uint32_t kNativeQueueTakeoverFullMaxTransparentHighOpaque =
    12288;

// Opaque 排序优化：
// 当队列已按比较器有序时跳过排序，减少 Flush 热路径开销。
inline constexpr bool kNativeQueueSkipSortIfAlreadySorted = true;
// 仅对中小批次做“有序性预检”，避免大批次“预检 + 排序”双重成本。
inline constexpr uint32_t kNativeQueueSkipSortCheckMaxCount = 8192;

// 保守接管模式：
// - 不做“全量接管”，仅在满足安全窗口时接管 Opaque Flush；
// - 目标是在不引入透明队列兼容风险的前提下，先回收一部分 Flush CPU；
// - 建议与 kNativeQueueTakeoverEnabled 二选一使用（full takeover 优先级更高）。
inline constexpr bool kNativeQueueTakeoverConservativeEnabled = false;

// 全量接管时，透明队列优先调用原生 FlushTransparent(0x138210)。
// 目的：
// 1) 降低透明材质排序/状态回归风险（例如隐身披风闪烁）；
// 2) 透明链路保持与原版一致，仅接管 Opaque 热路径。
inline constexpr bool kNativeQueueTakeoverUseNativeTransparentFlush = true;

// 保守接管触发阈值：Opaque 批次数达到该值才尝试接管（避免小场景负优化）。
inline constexpr uint32_t kNativeQueueTakeoverConservativeMinOpaque = 8;

// 无透明队列时的最小 Opaque 门槛（可低于 kNativeQueueTakeoverConservativeMinOpaque）。
// 目的：提升“无透明小批次”的接管命中率，减少回落原生路径的固定开销。
inline constexpr bool
    kNativeQueueTakeoverConservativeEnableSmallOpaqueNoTransparent = true;
inline constexpr uint32_t
    kNativeQueueTakeoverConservativeMinOpaqueNoTransparent = 1;

// 保守接管上限：超过该值时回退原生路径（防止异常队列布局触发风险）。
inline constexpr uint32_t kNativeQueueTakeoverConservativeMaxOpaque = 10000;

// 保守接管是否要求“透明队列为空”。
// 说明：现已补齐透明队列关键地址与分发函数，可按需放宽该条件。
inline constexpr bool kNativeQueueTakeoverConservativeRequireNoTransparent =
    false;

// 保守接管是否允许处理透明队列（当 transparentCount>0 时）。
// 仅在透明分发函数地址全部可用时生效。
inline constexpr bool kNativeQueueTakeoverConservativeAllowTransparent = true;

// 保守接管透明队列上限（防止极端场景透明条目过多导致风险）。
inline constexpr uint32_t kNativeQueueTakeoverConservativeMaxTransparent = 10000;

// 透明分级策略：
// - 当 transparentCount 超过该阈值时，保守接管直接回退原生；
// - 目标是在高透明压力场景优先保证稳定性，避免接管路径出现材质/排序回归。
inline constexpr uint32_t
    kNativeQueueTakeoverConservativeMaxTransparentForTakeover = 4096;

// 高 Opaque 压力下允许更高透明阈值（透明分级接管）。
// 仅在 Opaque 达到门槛时生效，避免小场景误开导致排序/材质风险扩大。
inline constexpr uint32_t
    kNativeQueueTakeoverConservativeHighOpaqueThreshold = 96;
inline constexpr uint32_t
    kNativeQueueTakeoverConservativeMaxTransparentForTakeoverHighOpaque = 8192;

// 透明分级策略：
// - 当存在透明队列时，提高 Opaque 最小门槛，避免“小批次 + 透明开销”负优化。
inline constexpr uint32_t
    kNativeQueueTakeoverConservativeMinOpaqueWhenTransparent = 24;

// 保守接管命中/回退统计（低频日志）。
// 性能优先默认关闭，排障时再开启。
inline constexpr bool kNativeQueueTakeoverConservativeStatsLogging = false;
inline constexpr uint32_t kNativeQueueTakeoverConservativeStatsIntervalCalls =
    12000;

// RenderQueue 诊断统计（BatchMergeStats、BatchMerger 分析）开关。
// 默认关闭：避免每批次原子计数/统计汇总带来的额外 CPU 开销。
inline constexpr bool kNativeRenderQueueDiagnosticStatsEnabled = false;

// 渲染队列自动合批（Instancing）实验开关
// 注意：该路径会改变批次合并与状态切换时机，若出现层级/TeamColor 问题请先关闭。
inline constexpr bool kNativeQueueAutoInstancingEnabled = false;

// ========================================================================
// RenderBatch_Submit 复现（Native Renderer Phase 2 起点）
// ========================================================================
// 是否启用 RenderBatch_Submit 的复现逻辑（替换游戏内部提交）
// 注意：该路径仍属实验性，建议先在小场景验证再扩大范围。
inline constexpr bool kNativeRenderBatchSubmitEnabled = false;

// RenderBatch_Submit 诊断统计（定期输出各类跳过原因）
// 性能优先默认关闭，排障时再开启。
inline constexpr bool kNativeRenderBatchDebugCounters = false;

// RenderBatch_Submit 优化：预计算可见层，避免首层多次前向探测
inline constexpr bool kNativeRenderBatchPrecomputeVisibilityEnabled = true;

// 预计算层数上限（防止极端模型导致栈开销过大）
inline constexpr uint32_t kNativeRenderBatchPrecomputeMaxLayers = 128;

// ========================================================================
// CWorldFrameWar3 / RenderScene 权威边界
// ========================================================================
// 说明：
// - 0x368480: 世界帧更新与渲染前准备；
// - 0x3681C0: CWorld_RenderScene，当前确认的权威世界渲染边界；
// - 该组 Hook 目前先用于“把真实世界边界接入状态机”，不直接改写原生渲染顺序。
inline constexpr bool kNativeWorldFrameBoundaryHooksEnabled = true;

// 若开启，则 BeforeUi 侧的 WORLD_RENDER_BEGIN / WORLD_RENDER_END 仅在
// 本帧确实命中过 CWorld_RenderScene 返回后才分发，避免继续把 BeforeUi
// 当成权威世界边界。
inline constexpr bool kNativeUseRenderSceneAsWorldEventAuthority = true;

// 是否记录最终可见 renderable manifest。
// 当前会同时记录：
// - RenderBatch_Submit 真正写入 main queue 的 opaque/main-queue 项
// - AUCTransparent_AddEntry 真正写入透明队列的条目
inline constexpr bool kNativeVisibleRenderableRegistryEnabled = true;

// 透明队列 identity/manifest hook 目前默认关闭。
//
// 2026-04-23:
// - 当前黑屏/早期崩溃已收敛到 RenderQueue_FlushTransparent(type4) family；
// - 而 AUCTransparent_AddEntry 的 ABI 证据在历史文档里存在冲突，继续默认挂载
//   identity hook 有把透明 entry 写坏的风险；
// - 在 ABI 重新用 IDA 收口前，先保留 opaque/main-queue registry，停用透明 registry
//   hook，优先恢复运行稳定性。
inline constexpr bool kNativeVisibleRenderableTransparentHookEnabled = false;

// ========================================================================
// JASS VM 追踪/优化实验
// ========================================================================
// 编译版内部测试 API 总闸门（默认关闭，避免发布版直接接触 JASSVM）。
inline constexpr bool kNativeInternalTestApiEnabled = false;
// 是否启用 JASS VM 主循环追踪（MainLoop / ExecuteInternal 分段）。
// 性能优先默认关闭，排障时再开启。
inline constexpr bool kNativeJassVmPerfTrackingEnabled = false;
// 是否启用 JASS VM 深层入口 Hook（ExecuteInternal/MainLoop）。
// 注意：
// - 这两个入口地址在不同小版本/补丁下可能发生漂移或落在函数中段；
// - 默认关闭，避免因地址/调用约定不匹配导致启动期崩溃；
// - 仅在已用 IDA 明确校验“函数起始 + 调用约定”后再开启。
inline constexpr bool kNativeJassVmDeepHooksEnabled =
    kNativeMainLoopCoverageAnalysisMode || false;
// JASS 细粒度 scope（/Orig 等）开关，默认关闭以减少热路径计时开销。
inline constexpr bool kNativeJassVmDetailedScopesEnabled = false;
// 是否输出 JASS VM 热路径日志（高频，默认关闭）。
inline constexpr bool kNativeJassVmVerboseLogging = false;
// 是否输出 JASS VM 返回码统计日志（低频，建议开启用于定位 timeout）。
inline constexpr bool kNativeJassVmResultStatsLogging = false;
// JASS VM 返回码统计日志输出间隔（按 MainLoop 调用次数）。
inline constexpr uint32_t kNativeJassVmResultStatsLogIntervalCalls = 4000;
// 是否覆盖 JASS 执行时间片预算（实验开关，默认关闭）。
inline constexpr bool kNativeJassOpBudgetOverrideEnabled = false;
// 覆盖值（原版常见为 300000）；仅在覆盖开关开启时生效。
inline constexpr int32_t kNativeJassOpBudgetOverrideValue = 300000;
// 是否启用 JASS opBudget 自适应（根据 timeout 比例动态调节预算）。
// 优先级低于 kNativeJassOpBudgetOverrideEnabled。
inline constexpr bool kNativeJassOpBudgetAdaptiveEnabled = false;
// 自适应预算初始值（建议从原版 300000 起步）。
inline constexpr int32_t kNativeJassOpBudgetAdaptiveInitial = 300000;
// 自适应预算最小值（防止过低导致频繁让步）。
inline constexpr int32_t kNativeJassOpBudgetAdaptiveMin = 220000;
// 自适应预算最大值（防止单次脚本执行过长导致帧卡顿）。
inline constexpr int32_t kNativeJassOpBudgetAdaptiveMax = 600000;
// 单次调节步长。
inline constexpr int32_t kNativeJassOpBudgetAdaptiveStep = 20000;
// 自适应统计窗口（按 MainLoop 调用次数）。
inline constexpr uint32_t kNativeJassOpBudgetAdaptiveWindowCalls = 2000;
// timeout 比例超过该阈值则提高预算。
inline constexpr double kNativeJassOpBudgetAdaptiveTimeoutHighRatio = 0.12;
// timeout 比例低于该阈值则尝试回落预算。
inline constexpr double kNativeJassOpBudgetAdaptiveTimeoutLowRatio = 0.01;

// JASS ExecuteJassFunction 入口统计（低频日志，帮助评估脚本入口成本）。
inline constexpr bool kNativeJassVmExecStatsLogging = false;
inline constexpr uint32_t kNativeJassVmExecStatsLogIntervalCalls = 20000;

// JASS Native 调用计划缓存（Task-4）：
// - Hook 开关：默认关闭，发布档不启用；
// - PlanCache 开关：用于“仅建计划/仅观测”；
// - FastInvoke 开关：开启后才尝试替代 ExecuteNativeFunction 热路径。
inline constexpr bool kNativeJassNativeCallHookEnabled = false;
inline constexpr bool kNativeJassNativeCallPlanCacheEnabled = false;
inline constexpr bool kNativeJassNativeCallFastInvokeEnabled = false;
inline constexpr bool kNativeJassNativeCallStatsLogging = false;
inline constexpr uint32_t kNativeJassNativeCallStatsInterval = 2000;
inline constexpr uint32_t kNativeJassNativeCallMaxArgs = 64;
inline constexpr uint32_t kNativeJassNativeCallCacheCapacity = 4096;
// 诊断：直接调用 GetFloatGameState 实现入口的早期探针。
// 已确认在 runtime ready 但地图未正式开始前直接探测风险较高，默认关闭。
inline constexpr bool kNativeDirectGetFloatGameStateProbeEnabled = false;
inline constexpr uint32_t kNativeGameStateTimeOfDayArg = 2u;
// 运行期在正式进图后 direct c_call GetFloatGameState。
// 当前实现按原生返回的 32 位 real 位模式解码，不再按 x87 float 返回值读取。
inline constexpr bool kNativeDirectGetFloatGameStateRuntimeEnabled = true;

// ========================================================================
// MainLoop / WinAPI 等待链路追踪
// ========================================================================
// 是否安装主线程 Wait/Sleep 系列 WinAPI Hook（用于拆分 Untracked）。
// 该组 Hook 覆盖面广，会引入可观额外开销，性能优先默认关闭。
inline constexpr bool kNativeMainThreadWaitHookEnabled =
    kNativeMainLoopCoverageAnalysisMode || false;
// 是否启用深层内核等待 Hook（WaitForMultiple/NtWait*）。
// 注意：该组 API 覆盖面很广，调试阶段建议按需开启，出现兼容/稳定性问题可快速关闭。
inline constexpr bool kNativeMainThreadWaitDeepHookEnabled =
    kNativeMainLoopCoverageAnalysisMode || false;
// Wait/Sleep 统计日志（低频）。
inline constexpr bool kNativeMainThreadWaitStatsLogging = false;
// Wait/Sleep 统计日志间隔（按主循环 Pump 调用次数）。
inline constexpr uint32_t kNativeMainThreadWaitStatsIntervalPumpCalls = 2000;

// 主循环链路统计日志（Pump/Dispatch/Callback 平均耗时）。
inline constexpr bool kNativeMainLoopStatsLogging = false;
// 主循环统计日志间隔（按 Pump 调用次数）。
inline constexpr uint32_t kNativeMainLoopStatsIntervalPumpCalls = 2000;

// 主循环深度阶段 Hook（Engine 内核函数）：用于拆分 Remaining Untracked。
// 仅做计时透传，不改变执行逻辑。该组 Hook 高频且有额外开销，性能优先默认关闭。
inline constexpr bool kNativeMainLoopDeepPhaseHookEnabled =
    kNativeMainLoopCoverageAnalysisMode || false;

// 诊断期阻止 War3 在切出窗口时自动暂停，避免 shadow/pose cadence 日志被前台切换污染。
inline constexpr bool kAutoTestDisableGamePause = true;

// 调试：忽略 CullTable 可见性（仅用于定位“漏渲染”原因）
inline constexpr bool kNativeRenderBypassCull = false;

// 调试：忽略 Layer 可见性（仅用于定位“漏渲染”原因）
inline constexpr bool kNativeRenderBypassLayerVisibility = false;

// 调试：忽略 MeshFlag（不设置 bit0，也不触发“首层后 break”）
// 用于验证“多层对象被提前截断”的可能性。
inline constexpr bool kNativeRenderIgnoreMeshFlag = false;

// ========================================================================
// Hook 快速路径（高频 Dispatch）
// ========================================================================
// 是否启用 Dispatch 快速路径（跳过 ExecBatchProcessor 桥接逻辑）
// 仅在无需对象追踪/Probe/调试模式时生效。
inline constexpr bool kNativeHookFastPathEnabled = true;

// ------------------------------------------------------------------------
// Hook 快速路径兼容开关（供新分域模块使用）
// ------------------------------------------------------------------------
// 无追踪需求时直接跳过桥接逻辑。
inline constexpr bool kNativeHookFastPathSkipBridgeWhenNoTracking = true;
// 对非世界对象标签是否跳过桥接。
inline constexpr bool kNativeHookFastPathSkipBridgeForNonWorldTag = true;
// Dispatch 局部合并：复用同 renderablePart 的 ExecBatch Begin/End 上下文，
// 降低 Common/Special 热路径桥接开销。
inline constexpr bool kNativeDispatchLocalContextMergeEnabled = false;
// Special(type3) 路径是否启用局部合并。
// 透明/特效链路语义更复杂，默认关闭，仅在专项验证后再打开。
inline constexpr bool kNativeDispatchLocalContextMergeSpecialEnabled = false;
// Dispatch 局部合并统计日志（低频）。
inline constexpr bool kNativeDispatchLocalContextMergeStatsLogging = false;
// 统计日志间隔（按 Dispatch 调用次数）。
inline constexpr uint32_t kNativeDispatchLocalContextMergeStatsInterval = 50000;
// Dispatch Tag/Stage 线程本地缓存：复用同 renderablePart 的 tracker 查询结果，
// 降低高频 GetTagStage 哈希探测开销。
// 当前实测命中率偏低（部分场景接近 0%），默认关闭以避免额外缓存开销。
inline constexpr bool kNativeDispatchTagStageCacheEnabled = true;
// Dispatch Tag/Stage 缓存统计日志（低频）。
inline constexpr bool kNativeDispatchTagStageCacheStatsLogging = false;
// 缓存统计日志间隔（按查询调用次数）。
inline constexpr uint32_t kNativeDispatchTagStageCacheStatsInterval = 100000;
// RenderQueue 可读范围校验结果缓存。
inline constexpr bool kNativeTrackQueueReadableRangeCacheEnabled = true;
// Stage->Tag 映射配置：0=实测映射，1=传统映射。
inline constexpr uint32_t kNativeStageTagProfile = 0;
// 是否按 groupIdx 对 WorldObjects 做精确标记。
inline constexpr bool kNativeTagWorldByGroupIdx = false;
// SceneSubmitBatch 计数日志（高频，默认关闭）。
inline constexpr bool kNativeSceneSubmitBatchCounterLogging = false;
// WorldObjects 组是否始终收集对象（调试用）。
inline constexpr bool kNativeWorldGroupAlwaysCollectObjects = false;
// 保守合并提交开关（实验路径）。
inline constexpr bool kNativeConservativeMergedSubmitEnabled = false;
inline constexpr bool kNativeConservativeMergedSubmitAllowNoObjectTracking =
    false;

// Hook 热路径详细日志（高频，默认关闭）
inline constexpr bool kNativeHookHotpathVerboseLogging = false;

// Native Patch 级别日志（默认关闭，避免污染热路径）。
inline constexpr bool kNativePatchVerboseLogging = false;
// 队列为空时是否跳过 FlushAndReset 原始调用。
// 开启后可减少“空 flush”成本，通常对 CPU 帧时间有正向收益。
inline constexpr bool kNativePatchSkipFlushWhenQueueEmpty = false;
// UI/FPS 覆盖：是否启用 MAX_FPS 覆盖写入。
inline constexpr bool kWar3UiOverrideMaxFpsEnabled = true;
// UI/FPS 覆盖目标值（仅在 kWar3UiOverrideMaxFpsEnabled=true 时生效）。
inline constexpr uint32_t kWar3UiMaxFpsOverrideValue = 1000u;
// UI/FPS 覆盖：是否写入刷新率配置项。
// 默认关闭：避免把游戏刷新率选项强制回写到系统当前值，影响用户手动改分辨率/刷新率。
inline constexpr bool kWar3UiOverrideRefreshRateEnabled = false;
// UI 域是否重复安装 GetD3d9Parameters Hook。
// 默认关闭：生命周期模块已安装同名 Hook，避免重复安装带来的兼容风险。
inline constexpr bool kWar3UiInstallD3d9ParamsHookEnabled = false;
// 是否强制 PresentationInterval=IMMEDIATE（生命周期/UI 参数 Hook 生效点）。
inline constexpr bool kWar3ForceImmediatePresentEnabled = true;
// 性能跟踪细粒度开关（用于 PerfMonitor 子分段）。
inline constexpr bool kNativeOptimizationPerfTrackingEnabled =
    kNativeMainLoopCoverageAnalysisMode || false;

// 超级快速路径：跳过 FlushSortedItems 中的冗余安全检查
// 原版游戏不做这些检查，启用后性能更接近原版
inline constexpr bool kNativeFlushUnsafePathEnabled = false;

// ========================================================================
// RenderQueueTracker 快速路径（Tag/Stage/对象缓存）
// ========================================================================
// 是否启用 RenderQueueTracker 的快速查找表（降低 Dispatch 热路径成本）
inline constexpr bool kNativeRenderQueueFastTrackerEnabled = true;

// 快速表容量（必须是 2 的幂；过小会降低命中率）
inline constexpr uint32_t kNativeRenderQueueFastTrackerCapacity = 1u << 18;

// 是否缓存 RenderObjectInfo（避免 ExecBatch 每次查询 Registry）
inline constexpr bool kNativeRenderQueueCacheObjectInfoEnabled = true;

// 诊断态总闸门：关闭我们自己的渲染接管模块，但尽量保留 Hook/桥接/追踪骨架。
// 用途：
// 1) 排查“持续卡顿是否由渲染接管链本身引起”；
// 2) 让对象身份/队列桥接仍可继续存在，避免把问题和渲染模块一起全掐掉后失去对照。
// 关闭后会停用：
// - BeforeUi 插入与相关 pass（阴影/描边/AA/SSAO/后处理）
// - ShadowCapture 热路径
// - war3shader render listener 的帧事件派发
inline constexpr bool kWar3RenderModuleTakeoverEnabled = true;

// ========================================================================
// Runtime profile 编译期分块排查开关
// ========================================================================
// 这组开关会被 war3_runtime_profile.cpp 直接编译进 runtime disabledModules。
// 改这里以后必须重新编译 d3d9.dll。
//
// 推荐排查顺序：
// 1) 先保持当前默认：DisableRenderInterference=true,
//    DisableSemanticData=false。
//    作用：关闭 shadow/postfx/AA/SSAO/render queue 等渲染层干涉，但继续保留
//    上层模型/pose/semantic 数据采集，用来判断“上层数据链本身是否卡”。
// 2) 若仍卡，把 DisableSemanticData 改为 true。
//    作用：连上层模型/pose/manifest/contract 数据链也关掉。
// 3) 若不卡，逐步把 DisableShadowStack / DisablePostFxStack /
//    DisableRenderInterference 改回 false，定位具体是哪一层拖慢。
//
// 注意：这组编译期禁用优先级高于 DXVK_WAR3_PROFILE/DXVK_WAR3_DISABLE；
// 也就是说，设为 true 后环境变量不能把该模块重新打开。
inline constexpr bool kWar3RuntimeConfigDxvkOnlyBaseline = false;

// 当前给用户手测的默认状态：先关全部渲染层干涉，保留 semantic.data。
inline constexpr bool kWar3RuntimeConfigDisableRenderInterference = false;

// 单独关闭 CSM 阴影链：shadow.capture/map/receiver/taa。
inline constexpr bool kWar3RuntimeConfigDisableShadowStack = false;

// 单独关闭后处理链：postfx/ssao/aa。
inline constexpr bool kWar3RuntimeConfigDisablePostFxStack = false;

// 单独关闭上层语义数据链：runtime model hook / pose / visible manifest /
// ShadowRuntimeContractCache / semantic scene/native semantic validation。
inline constexpr bool kWar3RuntimeConfigDisableSemanticData = false;

// 上层 semantic.data 子模块二分：
// 当前矩阵第四刀：打开 model/resource + pose + attachment + frame registry，
// 继续关闭 contract / consumer。若这一刀开始卡，问题集中在 registry
// begin/end 发布或 registry 数据结构维护；若不卡，再继续打开 contract。
inline constexpr bool kWar3RuntimeConfigDisableSemanticModelHooks = false;

// 仅关闭 CGeoset/CModelData 资源捕获子路径，保留 model hook 模块和
// runtime model owner 生产链。用于确认低压图 8 FPS 是否来自
// CreateGeosetFromRawArrays 后的 resource cache 写入/几何拷贝。
inline constexpr bool kWar3RuntimeConfigDisableSemanticGeosetResourceCapture =
    false;

// 仅关闭重型 pose 相关 hook（sprite frame / attachment pose 等）。
// 当前主路径改为 contract capture 阶段按 visible runtimeModel 直读 CModel
// palette/world transform。高频 SpriteFrameUpdate hook 默认退出主路径，避免
// semantic.data 在 SpriteFrameUpdate 上做 registry storm。
inline constexpr bool kWar3RuntimeConfigDisableSemanticPoseHooks = true;
// 上层阴影仍需要最终矩阵发布点：RuntimeMatrixRangeCopy 给 PoseRegistry
// 提供同帧 source-range palette，RuntimeMatrixWrite 给 renderablePart slot
// 提供引擎已经混合好的 group palette。它们比 SpriteFrame/attachment hook
// 轻得多，也正是语义阴影摆脱 VB/IB 捕获后的生产数据源。
inline constexpr bool kWar3RuntimeConfigEnableSemanticMatrixPublisherHooks =
    false;
inline constexpr bool kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled =
    true;

// Pose producer 关闭时是否仍安装 SpriteFrameUpdate 系列 Hook 来补 runtime
// resource。当前性能二分证明该热路径可能在无 pose 情况下造成未计入的
// semantic.data 卡顿，默认禁止，后续改成按 visible manifest 延迟补齐。
inline constexpr bool kWar3RuntimeConfigInstallSpriteFrameHooksWithoutPose =
    false;

// Phase 7.47 dt gate probe（独立诊断开关，只用于调查阴影 pose 卡顿）：
// 打开后，即便 pose hooks 被禁用，仍会安装
// CSpriteUber_PreRenderAndUpdatePosePalette_{Full,Mini,Lite,MiniLite} 四个
// hook。hook 实现只额外做一行 dt 分桶计数，不触发原来的 pose/identity
// registry 写入，避免 semantic storm。
// 也可通过环境变量 DXVK_WAR3_SPRITE_UBER_DT_PROBE 在运行时开启。
inline constexpr bool kWar3RuntimeConfigInstallSpriteUberDtProbeHooks = false;

// 仅关闭 attachment/local-point/attached-effect 相关 hook。
// 用于验证 local-point output / child runtime contract 是否是高开销点。
inline constexpr bool kWar3RuntimeConfigDisableSemanticAttachmentHooks = false;

// local-point output 的深度逆向探针会扫描 context/argBlock/父子 runtime。
// 默认只保留生产快路径；需要继续逆向 owner/child 映射时再临时打开。
inline constexpr bool kWar3RuntimeConfigSemanticAttachmentHeavyProbeEnabled =
    false;

// 关闭 visible manifest / resource / pose / object registry 的 begin/end 发布。
inline constexpr bool kWar3RuntimeConfigDisableSemanticFrameRegistries = false;

// semantic.data 性能二分：只禁用可见渲染清单写入，保留其它 frame
// registry 生命周期。用于确认卡顿是否集中在 VisibleRenderableRegistry。
inline constexpr bool
    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites = false;

// semantic.data 性能二分：可见渲染清单仍写入，但不在热路径做 identity /
// resource / geoset 的多级补全。用于区分“写一条记录”与“Finalize
// 补全链”哪个是主成本。
inline constexpr bool
    kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites = true;

// visible manifest 轻量写入后的帧末基础补全。只补 renderablePart 上的
// sceneNode / meshData，并重建索引；禁止在 register* 热路径做多级补全。
inline constexpr bool
    kWar3RuntimeConfigSemanticVisibleEndFrameBasicHydrate = true;
// 当前 semantic-only caster 必须从真实 visible renderable 获得
// `RenderablePart/MeshData -> runtimeModel/modelResource/geoset` 绑定。这里在
// EndFrame 做 O(visible) 轻量补全，避免在 Dispatch 热路径做多 registry join，
// 也避免继续依赖 RootUnitSupplement 的非当前 geoset 猜测。
inline constexpr bool
    kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate = true;
// Performance-safe static caster hydration: keep hot-path visible writes light,
// but let Building/Destructible records resolve model/geoset metadata once at
// EndFrame so full-scene semantic shadows can include static objects.
// Keep this off in the current units-only semantic scene path: an attempted
// diagnostic hydrate in that mode was proven unsafe on 2026-04-30 because it
// starved semantic skinned scene submission and produced a crash dump.
inline constexpr bool
    kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate = false;

// render queue 刚提交的 renderablePart 属于本帧 War3 自己正在消费的结构，
// 可以在 visible hydrate 阶段直读固定槽位，避免每条记录触发 VirtualQuery。
// 若后续遇到异常，可单独关掉退回 SafeReadPtrFast。
inline constexpr bool
    kWar3RuntimeConfigTrustVisibleRenderablePartPointers = true;

// visible manifest 轻量模式下，register* 热路径只追加 records，不维护多张
// unordered_map；索引统一在 endFrame hydrate 后重建。query 侧仍有线性兜底，
// 避免少量同帧查询失效。
inline constexpr bool
    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild = true;

// 数据层性能模式默认不在 endFrame 全量重建 visible manifest 多索引表。
inline constexpr bool
    kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame = false;

// semantic consumer 已经成为主路径后，query 侧不再是“少量”访问：如果完全
// 不维护索引，queryBy* 会在 capture/consumer 热路径反复线性扫 records。
// 这里只在 appendRecord 时维护热查询索引，不做 endFrame 全量重建。
inline constexpr bool
    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes = false;

// semantic.data 性能模式：禁止 registry 在 endFrame 对全量历史表做
// lastSeenFrame 扫描。热路径记录写入时已经携带当前 frameNumber；帧末全表
// 刷新只适合作为诊断，不应该成为默认数据层成本。
inline constexpr bool
    kWar3RuntimeConfigDisableSemanticRegistryEndFrameSweeps = true;

// semantic.data 性能二分：保留 VisibleRenderableRegistry 的正常记录写入，
// 但按模块关闭 FinalizeVisibleRecord 内部的重补全阶段。
inline constexpr bool
    kWar3RuntimeConfigDisableSemanticVisibleFinalizeIdentityResolve = true;
inline constexpr bool
    kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata = true;
inline constexpr bool
    kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata = true;
inline constexpr bool
    kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery = true;

// ========================================================================
// 直读 CModel Pose 快路径（绕过 Registry 链路）
// ========================================================================
// 启用后，SpriteFrameUpdate Hook 回调跳过所有 Registry 写入。
// 注意：这个早期实验缓存尚未接入 ShadowPoseStore 消费端，不能作为默认
// 主路径；真正的直读主路径在 ShadowRuntimeContractCache::captureLiveState。
inline constexpr bool kWar3SemanticDirectCModelPoseEnabled = false;

// 关闭 ShadowRuntimeContractCache::captureLiveState()。
inline constexpr bool kWar3RuntimeConfigDisableSemanticContractCapture = false;

// 关闭 semantic scene/native semantic consumer，但保留数据采集。
inline constexpr bool kWar3RuntimeConfigDisableSemanticConsumer = false;

// semantic.data 子模块的编译期有效状态。
// 重要：这些 effective 开关用于阻止“producer 已关闭、consumer 仍追帧”
// 造成的缺数据 build storm。runtime profile / DXVK_WAR3_DISABLE 仍会在调用侧
// 额外叠加判断。
inline constexpr bool kWar3RuntimeConfigSemanticModelProducerEffective =
    !kWar3RuntimeConfigDisableSemanticData &&
    !kWar3RuntimeConfigDisableSemanticModelHooks;
inline constexpr bool kWar3RuntimeConfigSemanticPoseProducerEffective =
    kWar3RuntimeConfigSemanticModelProducerEffective &&
    !kWar3RuntimeConfigDisableSemanticPoseHooks;
inline constexpr bool
    kWar3RuntimeConfigSemanticMatrixPublisherPoseEffective =
    kWar3RuntimeConfigSemanticModelProducerEffective &&
    kWar3RuntimeConfigEnableSemanticMatrixPublisherHooks;
inline constexpr bool kWar3RuntimeConfigPreferSemanticRuntimePoseUpdatePalette = false;
inline constexpr bool kWar3RuntimeConfigSemanticRuntimePoseUpdateEffective =
    kWar3RuntimeConfigSemanticModelProducerEffective &&
    !kWar3RuntimeConfigDisableSemanticPoseHooks;
inline constexpr bool kWar3RuntimeConfigSemanticRuntimeMatrixWriteEffective =
    kWar3RuntimeConfigSemanticModelProducerEffective &&
    kWar3RuntimeConfigEnableSemanticMatrixPublisherHooks;
inline constexpr bool kWar3RuntimeConfigSemanticAttachmentProducerEffective =
    kWar3RuntimeConfigSemanticPoseProducerEffective &&
    !kWar3RuntimeConfigDisableSemanticAttachmentHooks;
inline constexpr bool kWar3RuntimeConfigSemanticFrameRegistriesEffective =
    kWar3RuntimeConfigSemanticModelProducerEffective &&
    !kWar3RuntimeConfigDisableSemanticFrameRegistries;
inline constexpr bool kWar3RuntimeConfigSemanticContractCaptureEffective =
    kWar3RuntimeConfigSemanticFrameRegistriesEffective &&
    !kWar3RuntimeConfigDisableSemanticContractCapture;
inline constexpr bool kWar3RuntimeConfigSemanticConsumerEffective =
    kWar3RuntimeConfigSemanticContractCaptureEffective &&
    !kWar3RuntimeConfigDisableSemanticConsumer;

// 二分诊断：StormBreaker/TLSF 大块接管总闸门。
// 关闭后：
// - 不安装 Storm.dll alloc/free/realloc/getsize Hook
// - 不启用大块 redirect
// 用途：验证“超大地图长时间节奏性卡顿”是否来自早期内存接管链。
inline constexpr bool kWar3StormBreakerEnabled = true;

// 二分诊断：渲染域 Hook 安装子开关。
// 用途：
// - 将之前“一刀切”的渲染域总闸门拆成四个可单独验证的子域；
// - 便于确认到底是 UI、Render、Shadow 还是 runtime model 链导致卡顿。
inline constexpr bool kWar3UiHookEnabled = true;
inline constexpr bool kWar3RenderHookEnabled = true;
inline constexpr bool kWar3ModelHookEnabled = true;

// 阴影域二分：继续把 Shadow 域拆成更细的安装子域。
// 当前用户已确认“只要 Shadow 域开启就会卡”，所以下一步要确认
// 究竟是 Terrain 层、Projector 链，还是 UpdateWrite/静态清单链在拖慢大图。
inline constexpr bool kWar3ShadowTerrainHookEnabled = true;
inline constexpr bool kWar3ShadowProjectorHookEnabled = true;
inline constexpr bool kWar3ShadowUpdateHookEnabled = true;

// 阴影总闸门自动跟随三个子域，避免出现“子域全开但父开关仍是 false”
// 导致 Shadow 域实际上根本没有安装的误判。
inline constexpr bool kWar3ShadowHookEnabled =
    kWar3ShadowTerrainHookEnabled || kWar3ShadowProjectorHookEnabled ||
    kWar3ShadowUpdateHookEnabled;

// 汇总闸门：只要任一渲染子域开启，就视为“渲染域 Hook 已启用”。
inline constexpr bool kWar3RenderHookDomainsEnabled =
    kWar3UiHookEnabled || kWar3RenderHookEnabled ||
    kWar3ShadowHookEnabled || kWar3ModelHookEnabled;

// 是否启用“WorldObjectEntry_Render -> RenderQueue_AddBatch”身份桥接。
// 该桥把 worldObjectEntry / sceneNode / jHandle / rawcode 直接前推到
// RenderQueueTracker，避免 Dispatch 热路径再做 sceneNode 反查。
inline constexpr bool kNativeRenderIdentityBridgeEnabled = true;

// 半成品运行时阴影桥接（runtime shadow bridge v1）。
// 当前在超大图/超大量单位场景下仍可能反复触发高成本修复窗口，
// 导致长时间节奏性卡顿。默认关闭，回退到旧的 fallback 路径。
inline constexpr bool kShadowRuntimeBridgeEnabled = true;

// 半成品静态 persistent 阴影缓存。
// 当前仅在完整稳定的静态场景下收益明确；大图阶段先默认关闭，
// 避免与运行时桥接一起形成复杂交互。
inline constexpr bool kShadowPersistentGeometryCacheEnabled = true;

// runtime model hook（CreateSprite / runtimeModel 绑定探针）。
// 在关闭 runtime shadow bridge 的阶段，这条链默认一并关闭，避免继续
// 在后台做半成品 registry/pose 记账。
inline constexpr bool kShadowRuntimeModelHookEnabled = true;

// 2026-04-23 crash triage:
// - “整包 bootstrap provenance hooks”已经被证实会导致隔离桌面下黑屏/早崩；
// - 但完全关闭 bootstrap hooks 后，又会丢掉 anonymous attachment trio 的
//   owner-runtime 早期 provenance；
// - 因此这里改成“只恢复最小 early provenance 子集”的二分模式：
//   先只试 promote family，继续把 ctor / resolve / init-copy / child-link
//   这些更重的 early hooks 保持关闭。
inline constexpr bool kShadowRuntimeModelBootstrapHookEnabled = true;
inline constexpr bool kShadowRuntimeModelBootstrapPromoteHookEnabled = true;
inline constexpr bool kShadowRuntimeModelBootstrapResolveHookEnabled = false;
inline constexpr bool kShadowRuntimeModelBootstrapCtorHooksEnabled = false;
inline constexpr bool kShadowRuntimeModelBootstrapInitCopyHookEnabled = false;
inline constexpr bool kShadowRuntimeModelBootstrapChildLinkHookEnabled = true;

// runtime pose/palette hook（sprite prerender + runtime palette copy）。
// 上层阴影 consumer 需要这条链提供每帧稳定的 3x4 palette。
// 如需快速止血，可用 DXVK_WAR3_MODEL_POSE_HOOK=0 关闭。
inline constexpr bool kShadowRuntimePoseHookEnabled = true;

// child/attachment rigid contract:
// - local-point output -> root runtime -> child runtime
// - 默认优先只信 slotIndex -> child tag
// - sourceRecordIndex fallback 只在专项验证确认 slotIndex 长期 0 命中后再打开
inline constexpr bool kShadowAttachmentRigidContractEnabled = true;
inline constexpr bool kShadowAttachmentRigidAllowSourceRecordKeyFallback =
    false;

// 上层语义阴影 consumer：
// - 依赖 visible manifest + geoset resource cache + pose palette
// - 当前默认对 rigid 与 matrix-group skinned geoset 都优先走 authoritative
// - multi-group 的 runtime 矩阵按 War3 原生 `sub_6F12E200` 规则做等权合成
inline constexpr bool kUpperLayerShadowConsumerEnabled = false;

// 上层语义阴影观测模式：
// - 继续跑 resolve / stats / manifest / pose / resource 链
// - 但默认不直接发射新的 object shadow draw
// - 用于先稳定 AutoTest 与语义数据面，再逐步切回 authoritative
inline constexpr bool kUpperLayerShadowConsumerObserveOnly = true;

// 独立语义阴影核心验证：
// - 基于 contract cache 构建 ShadowRendererCore submission。
// - 现已作为 DXVK 验证基座默认主路径；需要回到旧诊断路径时可用
//   DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW=0 显式关闭。
inline constexpr bool kShadowSemanticCoreValidationEnabled = true;

// 语义阴影 frame -> DXVK scene 提交：
// - 在 BeforeUi 前直接把 ShadowRendererCore packet 落成 shadowInstances/shadowCasters。
// - 这是当前 object shadow 主路径。普通手动进图默认启用；如需二分旧路径，
//   可用 DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION=0 显式关闭。
inline constexpr bool kShadowSemanticCoreSceneSubmissionEnabled = true;
inline constexpr bool kShadowSemanticCoreSceneUnitsOnly = false;
inline constexpr bool kShadowSemanticCoreSceneSubmitDrawCapEnabled = false;
inline constexpr uint32_t kShadowSemanticCoreSceneSubmitDrawCap = 64u;
inline constexpr bool kShadowSemanticCoreSceneBootstrapCatchupEnabled = false;
inline constexpr uint32_t kShadowSemanticCoreSceneBootstrapCatchupMaxAttempts = 0u;
inline constexpr bool kShadowSemanticCoreSceneTailBoundaryFallbackEnabled = false;
inline constexpr bool kShadowSemanticCoreSceneEndFrameFlushEnabled = false;
// 2026-05 current draw contract correction:
// blocker skinned units do not reliably consume resource-side vertexGroupIndices.
// Formal semantic scene must allow the frame-local meshData contract through,
// otherwise we keep proving the right contract upstream and rejecting it during
// DXVK submission.
inline constexpr bool kShadowSemanticCoreAllowFrameLocalDynamicGeometry = true;
inline constexpr bool kShadowSemanticDispatchContractProbeEnabled = false;
inline constexpr bool kShadowSemanticCoreTreatFrameLocalDynamicMeshAsPreSkinned =
    false;
inline constexpr bool kShadowSemanticCorePreferFrameLocalDynamicMeshForSkinned =
    true;
// 只有当 semantic scene 的对象级 ownership 已验证稳定后，才允许在 capture
// 热路径前置旁路 legacy unit fallback。当前默认关闭，避免“只提交了一部分 packet，
// 却把整类单位 fallback 提前打没”。
inline constexpr bool kShadowSemanticCoreSceneBypassLegacyUnitCaptureEnabled =
    true;

// Formal semantic scene builds must consume the visible-renderable manifest.
// The old pose-resource preview seed pass enumerates the first few geosets of a
// posed runtime and was useful as a bootstrap probe, but it can submit hidden
// construction/scaffold or non-current submeshes once the visible path is live.
inline constexpr bool kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled =
    false;

// Formal semantic scene 的 skinned caster 必须来自当前可见 renderable 的
// primitive/index slice。若 live meshData 存在但无法解析 slice，继续退回整
// geoset 会把隐藏/建造/非当前子网格投进 ShadowMap，表现为“同一个局部 caster
// 阴影套到多名单位上”。保留开关便于现场二分。
inline constexpr bool
    kShadowSemanticCoreSceneRequireVisibleIndexSliceForSkinned = true;

// RootUnitSupplement 会从 runtime/modelResource 人工补 root geoset 记录；
// 这在早期“点亮第一帧”很有用，但 formal scene 一旦有 visible manifest，
// 它没有 meshData/renderablePart，无法携带当前可见 primitive slice，容易把
// 非当前子网格/局部 geoset 当成所有单位的 caster。正式路径先禁用，逼消费
// 层使用真实 visible renderable records。
inline constexpr bool kShadowSemanticCoreSceneRootUnitSupplementEnabled =
    false;

// 纯上层语义实验：
// - 对 object caster 不再回退到 legacy freeze/capture
// - 用于验证“完全不走捕捉冻结 VB/IB”时，当前链路能否直接画出对象阴影
// - 若需要快速回到混合模式，可改回 false
inline constexpr bool kUpperLayerShadowObjectNoCaptureFallbackEnabled = false;

// 是否输出 RenderQueueTracker 命中统计（低频日志）
inline constexpr bool kNativeRenderQueueTrackerStatsEnabled = false;

// 统计输出间隔（帧数）
inline constexpr uint32_t kNativeRenderQueueTrackerStatsInterval = 300;

// ========================================================================
// WorldObjects_RenderGroup 复现（Native Renderer Phase 2/3）
// ========================================================================
// 是否启用 WorldObjects_RenderGroup 的复现逻辑（替换对象遍历入口）
// 注意：该路径仍属实验性，若出现渲染缺失可先关闭。
inline constexpr bool kNativeRenderWorldGroupEnabled = true;

// ========================================================================
// Native Renderer Hook 接管（完全替换原版渲染流程）
// ========================================================================
// 是否启用 Native Renderer Hook 接管（替换游戏原生渲染函数）
// 注意：该路径当前仍会接管完整 RenderScene 主链；只有在 native-only
// takeover correctness 已单独验证通过时才应重新打开。默认关闭，避免主场景
// 贴图/材质被实验性 full-scene takeover 带坏。
inline constexpr bool kNativeRendererHookTakeoverEnabled = false;

// Native Renderer Hook 详细日志输出（用于调试Hook安装过程）
inline constexpr bool kNativeRendererHookVerboseLogging = true;

// 是否在Hook安装失败时继续运行（仅记录错误）
inline constexpr bool kNativeRendererHookContinueOnError = false;

// 在当前 DXVK 宿主的语义阴影 pass 中，直接执行 native D3D9 backend 的
// prepared draws，用于 Phase 5 的宿主内验证。
// 这不是最终 late-inject 独立宿主形态，但它可以在不接管完整 RenderScene
// 的前提下，先恢复“主画面正常 + 新阴影链仍可执行”的安全可用态。
inline constexpr bool kNativeRendererHostExecuteValidationEnabled = true;

// Stage-aware native semantic shadow validation:
// - 不开启完整 Native_RenderScene takeover；
// - 在原版 CWorld_DispatchStage 流程中，于 Stage2 后准备 semantic frame；
// - 于 Stage11 返回后、原版 flush 前执行已准备 native draws。
// 这条路用于验证“上层语义阴影能进入正确世界渲染时机”，避免继续依赖
// EndFrame/BeforeUi/tail-frame 作为功能前提。
inline constexpr bool kNativeSemanticShadowWorldStageValidationEnabled = false;
inline constexpr int kNativeSemanticShadowPrepareStage = 2;
inline constexpr int kNativeSemanticShadowRefreshPrepareStage = 10;
inline constexpr int kNativeSemanticShadowExecuteStage = 11;

// ========================================================================
// 对象追踪（RenderObjectRegistry/SceneCollector）
// ========================================================================
// 强制开启对象追踪（会关闭“仅追踪被关注句柄”的过滤，成本更高）
// 等价于 DXVK_WAR3_FORCE_OBJECT_TRACKING=1
inline constexpr bool kForceObjectTrackingEnabled = false;

// 追踪时是否强制走“完整解析”（HandleResolver/agentType/agentPtr 等）
// 等价于 DXVK_WAR3_OBJECT_TRACKING_FULL_RESOLVE=1
inline constexpr bool kObjectTrackingFullResolve = false;

// ========================================================================
// 渲染层 <-> 逻辑层桥接验证（FourCC）
// ========================================================================
// 是否启用“每种 FourCC 仅打印一次”的桥接验证日志。
// 用途：在渲染阶段验证 jHandle/unitPtr -> rawcode 的桥接准确性。
inline constexpr bool kBridgeRawcodeOneShotLogEnabled = false;

// 启用 FourCC 验证日志时，是否强制关闭 SceneCollector 过滤（收集全量对象）。
// 说明：仅用于验证期，开启后会增加对象追踪开销。
inline constexpr bool kBridgeRawcodeForceTrackAllEnabled = false;

// 是否启用 unit miss-mark（旧对比逻辑）
// 等价于 DXVK_WAR3_UNIT_MISS_MARK=1
inline constexpr bool kUnitMissMarkEnabled = false;

// ========================================================================
// Outline 调试（可选）
// ========================================================================
// 等价于 DXVK_WAR3_OUTLINE_ALL=1（注意：会对所有对象强制判定为描边目标）
inline constexpr bool kOutlineAllObjectsEnabled = false;

// 等价于 DXVK_WAR3_OUTLINE_FORCE=1（强制开启描边渲染开关）
inline constexpr bool kOutlineForceEnabled = false;

// ========================================================================
// Shadow Render Group Debug (ShadowMap 投射组调试)
// ========================================================================
inline bool kShadowRenderGroup0 = true;  // Units - 启用单位阴影
inline bool kShadowRenderGroup1 = true;  // Buildings (原版默认)
inline bool kShadowRenderGroup2 = true;  // Effects/Decorations - 启用装饰物阴影
inline bool kShadowRenderGroup3 = false; // ShadowCasters (修复选项，可选)

// ========================================================================
// Stage Debug Tool (渲染阶段调试工具)
// ========================================================================
inline bool kStageDebugEnabled = false; // 是否启用 Stage 调试模式
// Stage 开关：只渲染勾选的 stage
inline bool kStageDebug[25] = {
    true, true, true, true, true, // Stage 0-4
    true, true, true, true, true, // Stage 5-9
    true, true, true, true, true, // Stage 10-14
    true, true, true, true, true, // Stage 15-19
    true, true, true, true, true  // Stage 20-24
};

// ========================================================================
// 装饰物阴影控制 (Decoration Shadow Control)
// ========================================================================
// 装饰物是否投射阴影 (Stage 10: Decorations/Doodads/Trees)
inline bool kShadowRenderDecorations = true; // 默认开启

// ========================================================================
// ShadowMap 自适应更新（渲染层 CPU/GPU 优化）
// ========================================================================
// 在“高 caster + 相机/场景稳定”时，允许隔帧复用上一帧 ShadowMap。
// 注意：该策略偏向性能，若观察到阴影跟随延迟可关闭。
inline constexpr bool kShadowAdaptiveMapUpdateEnabled = false;
// 触发自适应的最小 caster 数。
inline constexpr uint32_t kShadowAdaptiveMapUpdateMinCasters = 16;
// 自适应跳帧周期：1=每帧更新，2=隔帧更新，3=每三帧更新一次。
inline constexpr uint32_t kShadowAdaptiveMapUpdatePeriod = 1;
// 当 caster 数极高时，提高复用周期，避免超大图里“一帧重建太久”。
inline constexpr uint32_t kShadowAdaptiveMapUpdateHighCasterThreshold = 768;
inline constexpr uint32_t kShadowAdaptiveMapUpdateHighCasterPeriod = 4;
inline constexpr uint32_t kShadowAdaptiveMapUpdateHugeCasterThreshold = 1536;
inline constexpr uint32_t kShadowAdaptiveMapUpdateHugeCasterPeriod = 6;
// 判定“相机稳定”的矩阵差阈值（view/proj 元素绝对差最大值）。
inline constexpr float kShadowAdaptiveMapUpdateCameraMaxDelta = 0.0005f;
// 判定“场景稳定”的 caster 数变化阈值。
inline constexpr uint32_t kShadowAdaptiveMapUpdateCasterDelta = 2;
// ShadowMap 分辨率自适应：
// Phase 7.68：视觉验证显示树叶/细枝阴影主要受有效 CSM 分辨率影响；旧策略会在
// replay geometry work 较高时把用户请求的 4096 静默降到 2048，导致 alpha
// cutout 细节被 texel 稀释。性能报告同时显示近期瓶颈不在 ShadowMap 分辨率本身，
// 因此默认保留请求分辨率，后续若需要再通过明确的质量档位重新引入。
inline constexpr bool kShadowAdaptiveResolutionEnabled = false;
inline constexpr uint64_t kShadowAdaptiveResolutionHighWork = 3000;
inline constexpr uint32_t kShadowAdaptiveResolutionHigh = 2048;
inline constexpr uint64_t kShadowAdaptiveResolutionHugeWork = 12000;
inline constexpr uint32_t kShadowAdaptiveResolutionHuge = 2048;
inline constexpr uint32_t kShadowAdaptiveResolutionMin = 2048;
// 自适应更新是否感知太阳运动（太阳移动时强制每帧更新 ShadowMap）。
inline constexpr bool kShadowSunMotionAwareAdaptiveUpdate = true;
// 太阳移动时是否自动关闭 Shadow TAA（避免历史混入导致拖影/波动）。
inline constexpr bool kShadowSunMotionAwareTaaDisable = false;
// Semantic dynamic unit shadows are driven by current-draw contracts. Until the
// receiver has per-caster motion vectors, temporal shadow history can blend an
// old/static shadow state with the current skinned pose and look like flicker.
inline constexpr bool kShadowDisableTaaForSemanticDynamicCasters = true;
// If the receiver reaches a frame with a non-world/invalid CSM candidate after
// a complete shadow map has already been rendered, keep the last-good map
// instead of clearing/redrawing it with a likely UI/portrait draw list.
inline constexpr bool kShadowHoldLastGoodMapOnInvalidCsm = true;
// Semantic current-draw producers can miss a transient frame at UI/scene
// boundaries. Hold a recent complete map briefly instead of flashing all
// shadows off for one bad replay frame.
inline constexpr bool kShadowHoldLastGoodMapOnTransientEmptyReplay = true;
inline constexpr uint32_t kShadowTransientEmptyReplayHoldFrames = 8;
// If semantic dynamic casters were seen recently, an empty replay frame is much
// more likely to be a producer/boundary miss than a true world-empty state.
// Hold the last-good map longer so a single bad cadence cannot globally flash
// every shadow off.
inline constexpr uint32_t kShadowSemanticDynamicEmptyReplayHoldFrames = 120;
// If the direct semantic caster identity set churns under current-draw pressure,
// publishing every newly selected subset makes the whole shadow map blink in
// sync. Keep the previous complete map through short identity churn windows;
// once object selection is stable, this gate becomes inactive automatically.
inline constexpr bool kShadowHoldLastGoodMapOnSemanticIdentityChurn = false;
inline constexpr uint32_t kShadowSemanticIdentityChurnHoldFrames = 120;
// 是否启用太阳时间量化（默认关闭，避免方向台阶更新带来的抽动感）。
inline constexpr bool kShadowSunTimeQuantizationEnabled = false;
// 级联剔除是否对建筑默认完全关闭（性能代价较大，发布档默认关闭该极端策略）。
inline constexpr bool kShadowCascadeCullDisableForBuildings = false;
// semantic 动态阴影已经进入 no-fallback 主路径后，单位也需要参与安全级联剔除，
// 否则 skinned caster 会在所有 cascade 中重复重放，dynamic pressure 直接 GPU bound。
inline constexpr bool kShadowCascadeCullDisableForUnits = true;
// 蒙皮单位的 bounds 来自 CModel pose/local bounds，当前保守放大后再做级联剔除：
// 半径偏大会少剔除但不会缺阴影；偏小则会造成远镜头/低 Z 处 caster 缺底。
inline constexpr float kShadowCascadeCullSkinnedRadiusScale = 1.55f;
inline constexpr float kShadowCascadeCullSkinnedExtraRadius = 160.0f;
inline constexpr float kShadowCascadeCullSkinnedExtraGuardNdc = 0.24f;
inline constexpr float kShadowCascadeCullSkinnedZExtraGuardNdc = 0.32f;
// 建筑按保守剔除处理时的包围球放大倍率与额外 NDC 保护带。
inline constexpr float kShadowCascadeCullBuildingRadiusScale = 1.35f;
inline constexpr float kShadowCascadeCullBuildingExtraGuardNdc = 0.10f;
// 超大 draw（索引/顶点阈值）按保守剔除处理时的放大参数。
inline constexpr float kShadowCascadeCullLargeDrawRadiusScale = 1.20f;
inline constexpr float kShadowCascadeCullLargeDrawExtraGuardNdc = 0.06f;
// 判定“太阳在移动”的最小角度阈值（单位：度）。
inline constexpr float kShadowSunMotionThresholdDeg = 0.05f;
// 太阳移动时是否强制关闭 stable snap（避免 texel snapping 台阶导致的抖动）。
inline constexpr bool kShadowDisableStableSnapWhenSunMoving = false;
// 太阳移动时是否强制切到 StableSphere 拟合（稳定优先，降低旋转敏感闪烁）。
inline constexpr bool kShadowForceStableSphereWhenSunMoving = false;
// 级联剔除 NDC 保护带（边缘闪烁抑制）；x/y 使用 ±(1+guard), z 使用 [-guard,1+guard]。
inline constexpr float kShadowCascadeCullGuardBandNdc = 0.10f;
// ShadowCapture 捕获前粗剔除，避免“先拷入 Arena、后在级联里剔掉”。
inline constexpr bool kShadowCaptureCoarseCullEnabled = false;
inline constexpr float kShadowCaptureCoarseCullDistanceScale = 1.10f;
inline constexpr float kShadowCaptureCoarseCullGuardNdc = 0.35f;
// 超大场景下 ShadowMap 重放 caster 数上限。
// 目标不是追求“所有单位都每帧吃满”，而是优先保证可交互性与核心阴影稳定。
inline constexpr bool kShadowReplayCasterCapEnabled = false;
inline constexpr uint32_t kShadowReplayCasterCap = 32;
// 超大图中，动态冻结路径每帧最多接受的 world/unit caster 数。
// 目标：优先避免“单帧几千个 fallback 冻结”造成的长时间卡死。
inline constexpr bool kShadowFallbackFreezeCountCapEnabled = false;
inline constexpr uint32_t kShadowFallbackWorldFreezeCountCap = 768;
inline constexpr uint32_t kShadowFallbackUnitFreezeCountCap = 512;
// 时间平滑是否启用“按 raw 采样自适应估速”；关闭后使用固定速度+误差回正（更平滑）。
inline constexpr bool kShadowTimeAdaptiveSpeedEnabled = true;
// 阴影运行统计日志（含矩阵信息）默认关闭，避免输出通道导致主线程周期性抖动。
inline constexpr bool kShadowRunStatsLogging = false;
inline constexpr uint32_t kShadowRunStatsLogIntervalFrames = 300;

// ========================================================================
// 路径阻断器诊断 (Path Blocker Debug)
// ========================================================================
inline bool kPathBlockerDebugEnabled = false; // 启用诊断日志
inline bool kPathBlockerHideEnabled = true;   // 隐藏路径阻断器渲染
// 开启路径阻断器隐藏时，是否强制开启桥接追踪（确保 ShadowCapture 能拿到 rawcode）。
inline constexpr bool kPathBlockerForceBridgeTrackingEnabled = false;
// “仅路径阻断器追踪”模式下的组掩码（bit0=group0, bit1=group1 ...）。
// 默认仅追踪 group0，减少 SceneCollector 在无描边/无 Bloom 时的全组扫描开销。
inline constexpr uint32_t kPathBlockerTrackingGroupMask = 0x1u;
// ========================================================================
// 阴影投影器过滤（建筑阴影拦截）
// ========================================================================
// 原生阴影默认模式：0=原样，1=仅雾/边界，2=完全禁用
inline constexpr uint32_t kNativeShadowDefaultMode = 1;

// 当原生阴影模式为“仅雾/边界/禁用”时，是否同步关闭 War3ShadowReceiver（CSM）
inline constexpr bool kNativeShadowDisableWar3ShadowReceiverWhenMode1 = false;

// 当原生阴影模式为“仅雾/边界/禁用”时，是否禁用 Outline 依赖的 ShadowReceiver
// 注意：关闭后描边可能失效（用于验证“建筑阴影来自 CSM”）
inline constexpr bool kNativeShadowDisableOutlineWhenMode1 = false;

// 当原生阴影模式为“仅雾/边界/禁用”时，是否完全跳过 ShadowCapture（避免 CSM
// 产生阴影）
// 默认必须关闭该开关：项目定位是“画质增强”，不应默认关闭核心阴影链路。
inline constexpr bool kNativeShadowDisableShadowCaptureWhenMode1 = false;

// 是否安装 ShadowPath_StaticStamp_Toggle(0x74E420) Hook。
// 该链路不经过 RegisterImage/ListA/ListB，是静态阴影写入的独立入口。
inline constexpr bool kNativeShadowStaticStampPathHookEnabled = true;

// mode=1 时是否屏蔽 ShadowPath_StaticStamp_Toggle 的 enable 写入。
inline constexpr bool kNativeShadowBlockStaticStampPathWhenMode1 = true;

// mode>=2 时是否屏蔽 ShadowPath_StaticStamp_Toggle 的 enable 写入。
inline constexpr bool kNativeShadowBlockStaticStampPathWhenMode2 = false;

// ShadowPath_StaticStamp_Toggle 调试日志（默认关闭）。
inline constexpr bool kNativeShadowStaticStampPathVerboseLogging = false;

// 是否拦截建筑阴影投影器（用于“仅保留雾/边界”的实验）
inline constexpr bool kNativeShadowBlockBuildingProjectorEnabled = false;

// 是否拦截所有投影器（用于快速验证阴影链路影响范围）
inline constexpr bool kNativeShadowBlockAllProjectorEnabled = false;

// 是否拦截 0x1DEE80 来源的投影器（可能为非建筑路径，需验证）
inline constexpr bool kNativeShadowBlockProjectorFromAltEnabled = false;

// 是否拦截 0x76D800 来源的投影器（Shadow_AddProjectorFromObject）
inline constexpr bool kNativeShadowBlockProjectorFromObjectEnabled = false;

// 是否拦截 0x76D790 来源的投影器（Shadow_AddProjectorSimple）
inline constexpr bool kNativeShadowBlockProjectorSimpleEnabled = false;

// 是否在 Projector Hook 热路径中写入 native shadow hint registry。
// 该路径会做 object -> unit/agent/handle 解析，并伴随 mutex + unordered_map
// 写入；在超大地图里是高频重路径，默认关闭，仅在确实需要 hint 语义时再开。
inline constexpr bool kWar3ShadowProjectorNativeHintEnabled = false;

// 是否在 Projector Hook 热路径中执行 FourCC 过滤。
// 说明：
// - 这条链会走 TryExtractShadowObjectFourCC -> Unit/Agent 解析；
// - 对超大地图而言，这是一个非常昂贵的高频热路径；
// - 当前默认关闭，只有在确实需要 PathBlocker/FourCC 精确拦截时再打开。
inline constexpr bool kNativeShadowProjectorFourCCFilterEnabled = false;

// 影子投影器调试日志（打印少量关键数据）
inline constexpr bool kNativeShadowProjectorVerboseLogging = false;

// 影子投影器低频统计日志（calls/blocked/path 分布）
inline constexpr bool kNativeShadowProjectorStatsLogging = false;

// 需要拦截的 UberSplat 关键字（用于建筑阴影）
// 说明：这些 key 通常来自单位数据表的 UberSplat 字段（例如 OLAR）。
inline constexpr const char *kNativeShadowBlockedUbersplatKeys[] = {"OLAR"};
inline constexpr uint32_t kNativeShadowBlockedUbersplatKeyCount =
    sizeof(kNativeShadowBlockedUbersplatKeys) /
    sizeof(kNativeShadowBlockedUbersplatKeys[0]);

// 需要拦截的四字码（LOS 阻断器等）
// 注意：运行时会做第二字符大小写归一化
inline constexpr uint32_t kNativeShadowBlockedFourCCs[] = {
    0x59546162u, // 'YTab'
    0x59546163u, // 'YTac'
    0x59547062u, // 'YTpb'
    0x59547063u, // 'YTpc'
    0x59546662u, // 'YTfb'
    0x59546663u, // 'YTfc'
    0x59546C62u, // 'YTlb'
    0x59546C63u, // 'YTlc'
};
inline constexpr uint32_t kNativeShadowBlockedFourCCsCount =
    sizeof(kNativeShadowBlockedFourCCs) / sizeof(kNativeShadowBlockedFourCCs[0]);

// ========================================================================
// 阴影更新链路拦截（Shadow_UpdateList_Run / Shadow_UpdateEntry_Write）
// ========================================================================
// 是否输出更新链路日志（回调地址/entry 指针）
inline constexpr bool kNativeShadowUpdateVerboseLogging = false;

// 是否直接阻断更新链路（用于验证“阴影是否来自更新链”）
inline constexpr bool kNativeShadowBlockUpdateListEnabled = false;

// 更新链路统计日志（低频）
inline constexpr bool kNativeShadowUpdateStatsLogging = false;

// 是否按回调地址阻断更新链路（用于精确过滤“建筑阴影回调”）
inline constexpr bool kNativeShadowBlockUpdateByCallbackEnabled = false;

// 需要阻断的回调地址数组（RVA，相对 Game.dll base）。
// 仅在 kNativeShadowBlockUpdateByCallbackEnabled=true 时生效。
inline constexpr uint32_t kNativeShadowBlockedCallbackRvas[] = {
    // 例如 0x73F7A0u
};
inline constexpr uint32_t kNativeShadowBlockedCallbackRvasCount =
    sizeof(kNativeShadowBlockedCallbackRvas) /
    sizeof(kNativeShadowBlockedCallbackRvas[0]);

// ========================================================================
// 阴影注册入口拦截（TerrainShadow_RegisterImageEntry）
// ========================================================================
// 是否安装注册入口 Hook（上游拦截，优先于 ListA/ListB 渲染末端过滤）
inline constexpr bool kNativeShadowRegisterImageHookEnabled = false;

// mode>=1 时是否拦截 StaticStamp 来源（0x74DB30）注册
inline constexpr bool kNativeShadowBlockStaticStampRegisterWhenMode1 = false;

// mode>=1 时是否拦截 EmitterStamp 来源注册
// - 函数入口：0x74DE40
// - RegisterImage 调用点返回地址：0x74DF55
// 说明：
// - 实机验证中该来源与单位地面贴花阴影复用链路高度相关；
// - 直接拦截会导致贴花纹理未正确注册，出现“黑色方块阴影”伪影；
// - 默认关闭，避免对动态单位阴影造成破坏。
inline constexpr bool kNativeShadowBlockEmitterStampRegisterWhenMode1 = false;

// 注册入口低频统计日志
inline constexpr bool kNativeShadowRegisterImageStatsLogging = false;

// 注册入口详细日志
inline constexpr bool kNativeShadowRegisterImageVerboseLogging = false;

// RegisterImage 来源分桶统计日志（按返回地址来源）。
inline constexpr bool kNativeShadowRegisterSourceStatsLogging = false;

// RegisterImage 来源详细日志（高频，默认关闭）。
inline constexpr bool kNativeShadowRegisterSourceVerboseLogging = false;

// RegisterImage 来源统计输出间隔。
inline constexpr uint32_t kNativeShadowRegisterStatsInterval = 4000;

// mode=1 严格策略开关：启用 owner-aware 决策规则。
inline constexpr bool kNativeShadowRegisterPolicyStrictMode1 = false;

// mode=1 是否直接屏蔽所有 RegisterImage 写入（极限诊断开关）。
// 用途：验证“可见阴影是否仍走 RegisterImage 路径”。
inline constexpr bool kNativeShadowRegisterBlockAllWhenMode1 = false;

// mode=1 时是否优先拦截 "ReplaceableTextures\\Shadows\\*" 与
// "Shadow/ShadowFlyer" 这类原生阴影纹理 key。
// 说明：
// - 该规则用于直接抑制建筑/可破坏物静态阴影主贴图路径；
// - 比拦截 Splats（地面贴花融合）更贴近“只去阴影”的目标。
inline constexpr bool kNativeShadowRegisterBlockShadowTextureKeyWhenMode1 = false;

// mode=1 时是否拦截 WithParams + UberSplat。
// 说明：
// - UberSplat 通常用于建筑与地面交界贴花，不等同于阴影本体；
// - 默认关闭，避免把“贴花融合”当成“阴影”误杀。
inline constexpr bool kNativeShadowRegisterBlockWithParamsUberSplatWhenMode1 =
    false;

// 是否启用 RegisterImage owner 类型过滤（Building/Destructible 精确拦截）。
inline constexpr bool kNativeShadowRegisterOwnerKindFilterEnabled = false;

// Unknown owner 是否启用 "argType={0,4} + 建筑样式 key" 兜底拦截。
// 关闭后 Unknown owner 始终放行（用于回滚 Round3 条件）。
inline constexpr bool kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled = false;

// ========================================================================
// 阴影列表A 过滤（仅雾/边界保留）
// ========================================================================
// 是否安装 ListA Hook（默认关闭，避免误伤雾/边界）
inline constexpr bool kNativeShadowListAHookEnabled = false;

// mode=1 时是否按 groupCountB 拦截组条目（默认关闭）
inline constexpr bool kNativeShadowListABlockGroupEntriesEnabled = false;

// 是否启用 ListA 纹理白名单（仅保留首批纹理）
inline constexpr bool kNativeShadowListAWhitelistEnabled = false;

// 白名单最大纹理数量（达到后拒绝新纹理）
inline constexpr uint32_t kNativeShadowListAWhitelistMaxTex = 40;

// 白名单调试日志（记录新增/拒绝纹理）
inline constexpr bool kNativeShadowListAWhitelistVerboseLogging = false;

// ListA 低频统计日志
inline constexpr bool kNativeShadowListAStatsLogging = false;

// ========================================================================
// 阴影列表B 过滤（静态建筑阴影链路）   有问题不要开
// ========================================================================
// 是否安装 ListB Hook（默认关闭，按需开启）
inline constexpr bool kNativeShadowListBHookEnabled = false;

// mode=1 时是否拦截 type=4（stage14 直调链路）
inline constexpr bool kNativeShadowListBBlockType4WhenMode1 = false;

// mode=1 时拦截 type=4 是否仅限 stage14 调用点（return RVA=0x007378FA）。
// 默认开启：避免把其它 type=4 的贴花路径一并误杀。
inline constexpr bool kNativeShadowListBType4Stage14OnlyWhenMode1 = false;

// mode=1 时是否拦截 type=3（静态阴影残留常见路径）
inline constexpr bool kNativeShadowListBBlockType3WhenMode1 = false;

// mode=1 时是否拦截全部 ListB（极限诊断开关）
inline constexpr bool kNativeShadowListBBlockAllWhenMode1 = false;

// mode>=2 时是否拦截全部 ListB
inline constexpr bool kNativeShadowListBBlockAllWhenMode2 = false;

// ListB 低频统计日志
inline constexpr bool kNativeShadowListBStatsLogging = false;

// ListB 详细日志
inline constexpr bool kNativeShadowListBVerboseLogging = false;

// ========================================================================
// Shadow_Update_WriteEntry Hook
// ========================================================================
// 是否安装更新写入入口 Hook（默认关闭，按需开启）
inline constexpr bool kNativeShadowUpdateWriteHookEnabled = false;

// ========================================================================
// 16位索引溢出防护（撕裂兜底）
// ========================================================================
// 是否启用 16bit Index 的过大批次拆分（避免索引越界撕裂）
inline constexpr bool kWar3IndexOverflowGuardEnabled = true;

// 单次 draw 允许的最大 index 数（16-bit）
inline constexpr uint32_t kWar3IndexOverflowGuardMaxIndices = 0xFFFFu;

// 输出拆分日志（用于确认是否触发）
inline constexpr bool kWar3IndexOverflowGuardVerboseLogging = false;

} // namespace dxvk::war3::internal
