# War3 渲染问题统一研究目录

## 当前阶段（2026-04-05）
- **代码稳定回退点**：`ea204b1`，作为当前进入下一阶段前的安全基线。
- **桥接现状**：
  - `runtime shadow bridge v1` 已落地；
  - “对象身份直传桥”已将 `WorldObjectEntry_Render -> RenderQueue_AddBatch -> RenderQueueTracker` 串通；
  - 动态单位当前仍以 **fallback 正确性优先**，尚未正式点亮动态 Pose Takeover 主路径。
- **研究结论收敛**：
  - 动态单位未来应走“静态模型资源 + 每帧 3x4 pose palette 更新”路线；
  - `RenderablePart + 0x108 = geosetIndex` 已可视为运行时 geoset 的直接键；
  - 生产级接入应优先收敛到 `CSpriteUber_PreRenderAndUpdatePosePalette` 返回时机，并避免高风险 dispatch 深链接入。
- **当前主阻塞**：
  - 仍需继续隔离 crash / UAF / stale pointer；
  - AutoTest 的 in-map 验证链还需要继续稳固；
  - 动态 Pose Takeover 进入正式落地前，必须先保证生命周期与崩溃证据链可靠。

## 相关研究
- [war3_model_runtime](../war3_model_runtime/README.md)：模型加载、实例绑定与动画/姿态运行时。
- [war3_shadow_semantic_cost](../war3_shadow_semantic_cost/README.md)：阴影语义追踪的热路径成本与降本方向。
- [war3_shadow_runtime_v2](../war3_shadow_runtime_v2/README.md)：模型资源 + 姿态更新的阴影运行时目标。

## 目录说明
- `01_batch_merge/README.md`：渲染对象批次合并（Queue Takeover + Instancing）研究与推进记录。
- `02_los_blocker_shadow/README.md`：路径阻断器（LOSBlocker）误入阴影采集问题研究与修复记录。
- `03_building_static_shadow/README.md`：建筑静态阴影在“关闭阴影”后仍渲染的问题研究与修复记录。
- `04_architecture_refactor/README.md`：Hook 框架/桥接层/运行时生命周期的重构方案与里程碑。
- `05_jass_vm_and_partial_batch_submit/README.md`：JASS VM 主循环画像与渲染层“局部合并提交”专项。
- `06_message_1_4_archive/README.md`：留言1~4夜间专项的统一归档（目标/实现/证据/验证清单）。
- `07_main_loop_deep_trace/README.md`：主循环深度追踪（GameMain/Engine Loop/阶段分解）与可观测点位。
- `08_waitgate_cpu_optimization/README.md`：WaitGate 语义澄清、主循环等待/活跃拆分与 CPU 降本路线。
- `09_2026_02_24_nightly_closeout/README.md`：本夜（第五轮+第六轮）优化与兼容收官总结。
- `10_p3_contract_static_gate/README.md`：P3 架构契约化与渲染优化静态门控（仅静态评估）。
- `07_mainloop_full_breakdown/README.md`：主循环全链路拆解（sub_6F05F710）+ 模块级计时补点 + 优化收益评估。
- `11_mainloop_round4_unknown_resolution/README.md`：第四轮 MainLoop 逻辑层未知项压缩、模块级耗时与优化计划。
- `12_round5_perf_matrix_150fps/README.md`：第五轮配置矩阵、150FPS 目标验证与上限探索结论。
- `12_mainloop_full_reverse/README.md`：MainLoop 函数级全量逆向、覆盖矩阵与 Unknown 归因拆分。
- `15_mainloop_jass_vm_thesis/README.md`：MainLoop + Jass/JassVM 统一定义论文（架构图、时序图、地址证据与工程化结论）。
- `16_storm_memory_hook_reverse/README.md`：`Storm.dll` 内存导出、头布局与大块接管约束的 ASM 逆向记录。
- `17_cworldframewar3_full_reverse/README.md`：`CWorldFrameWar3` 全量逆向（vtable、字段偏移、RenderScene/DispatchStage、类图与时序图）。
- `18_csprite_animation_attach_reverse/README.md`：`CSprite / CSpriteUber / 动画切换 / 特效挂点` 专题逆向。
- `19_blizzard_native_rendering_engine_full_perspective/README.md`：从暴雪原生引擎视角统一收口 `MainLoop / CWorldFrameWar3 / RenderQueue / CSprite / TerrainShadow / UI 3D` 全景。
- `20_renderqueue_dispatch_layer_reverse/README.md`：`RenderBatch_Submit / FlushSortedItems / Dispatch_Common / Dispatch_Special / fallback multipass` 深入逆向。
- `21_render_logic_bridge_optimization_notes/README.md`：渲染层对象到逻辑层 `CAgent/CUnit` 桥接的现状、热点和低损耗入口盘点。
- `22_cmodel_pose_palette_reverse/README.md`：`CModelData/CGeosetData -> matrix-group remap -> CModel 最终 3x4 pose palette -> attachment/runtimeModel` 专题逆向。
- `23_blob_shadow_lista_upstream_reverse/README.md`：`Blob 阴影 / ListA` 上游写入链专题逆向，收口 `StaticStamp / RegisterImage / UberSplat / ListA mixed layer` 的真实关系。
- `24_cdoodads_static_shadow_upstream/README.md`：**`CDoodads + CUnit + FogMask` 静态阴影完整逆向（v3）**。
  v1 误以为 CDoodads 是建筑阴影主治理点；v2 误以为 CUnit ShadowProjector 是。
  **v3 决定性发现**：建筑预渲染贴花阴影既不走 CDoodads 也不走 CUnit ShadowProjector，
  而是通过 `TerrainShadow_WriteMaskRegion (0x234710)` 直接修改 `CFogMaskTable` 的 16-bit mask grid
  （和战争迷雾、视野、路径阻挡共享）。这解释了为什么历史所有针对 RegisterImage / StaticStamp / ListA/B
  的拦截都不能消除建筑阴影（包括 AGENTS 第 57 条 `Mode1_BlockAllRegisterImage` 极限实验）。
  CDoodads `a6` mask 注入仍能关树木阴影；建筑阴影必须 hook `WriteMaskRegion` 按 type code bit
  做精细屏蔽（不能整体 return，否则同时关 fog/视野）。
- `../../plan/overnight_render_paper_2026_05_15/06_fogmask_static_shadow.md`：
  **24 文档 v3 之后的完整 FogMask 论文（2026-05-15 夜间无人值守新增）**。
  揭示真正的根因：
  (1) 4 个并行 mask layer 实际是 *2 mask + 2 elevation grid*：
      `+0x2C` clearMaskBase / `+0x30` setMaskBase / `+0x38` elevationMaskBase / `+0x3C` aboveCurrentMaskBase；
  (2) `WriteMaskRegion` 的 `a3 type code` 第 0..11 位是 *12 个 player slot 共享视野 mask*，
      不是"类型分桶"；
  (3) 真正决定"在哪份 mask 上写"的是对象 `+0x10C` 字段的 `mask idx`：
      `idx=0 fog / idx=1 LOS / idx=2 path / idx=3 shadow footprint / idx=4 flying`；
  (4) **静态阴影治理最干净方案**：hook `WriteMaskRegion` + 仅当 `*(a2+0x10C) == 3` 拦截，
      不影响 fog/LOS/path；
  (5) `CWidget_RegisterFootprintAndShadowMask (0x65A140)` 30+ caller 完整分桶；
  (6) `magic 0x2B5DB42C` 是 `CWidget` 类型签名，仅注册 widget 才有；
  (7) 4 次历史失败拦截尝试的反证表。
  ★ 所有结论已通过 IDA 写回（41 处 rename + 14 条 set_comments，全部 ok）。
- `24_crash_tracking_autotest/README.md`：War3 未处理异常 `.dmp + JSON` 崩溃摘要落盘，以及 AutoTest 对最近崩溃证据的回收。
- `native/README.md`：基于 ASM 的魔兽原生渲染链还原进度与优化方案草案。
- `14_2026_02_26_static_shadow_write_gate_closeout/README.md`：静态阴影“写入端五轮计划”收官报告（证据、验收与残余风险）。

## 统一目标
1. 减少 `FQ_Dispatch_Opaque` CPU 开销，恢复并超过基线 FPS。
2. 清除玩家不可见对象（尤其 LOSBlocker）对阴影结果的污染。
3. 屏蔽原生建筑静态阴影与项目阴影的叠加，改善整体观感。

## 当前状态（2026-02-25）
- 已新增 `17_cworldframewar3_full_reverse/README.md`：
  - 基于 IDA PRO MCP 完成 `CWorldFrameWar3` 主/次 vtable、关键字段偏移、`RenderScene -> DispatchStage -> TerrainShadow/WorldObjects` 调度链与 `CGameUI` 组合关系的专题逆向。
  - 已在 IDA 中落地 `【CWorldFrameWar3专题】` 标记、主函数重命名与核心地址注释。
- 已新增 `18_csprite_animation_attach_reverse/README.md`：
  - 基于外部资料 + IDA PRO MCP 交叉核对，补齐 `CSprite / CSpriteUber / CWidget / CEffect` 在“动画切换 + 特效挂点 + prerender”链上的高置信度字段与函数关系。
  - 已将 `CWidget + 0x28 -> CSprite*`、`CSprite` 动画环形队列、`CEffect` 挂点数组/目标哈希等结构落地到公用逆向头。
- 已新增 `19_blizzard_native_rendering_engine_full_perspective/README.md`：
  - 将 `CWorldFrameWar3`、`RenderQueue`、`CSprite/CAnimComplex`、`TerrainShadow`、`UI 3D / postprocess / overlay` 串成同一张原生引擎全景图。
  - 已补写 `RenderQueue_*`、`CModel_*`、`stage16/18/21` 尾链等函数名与中文作用注释回 IDA，后续可直接按名字继续深挖。
- 已新增 `20_renderqueue_dispatch_layer_reverse/README.md`：
  - 深挖 `SceneNode -> RenderablePart -> MeshData -> MeshInfo -> LayerState/LayerDispatch` 两级记录和 `Dispatch_Common/Special` 的真实参数顺序。
  - 这部分是未来“接管原生渲染层”最关键的事实基线。
  - 已进一步确认 `sub_6F0E35B0` 不是 draw、`sub_6F0E3550` 才是真正的底层 draw，且 `MeshData` 真实最小尺寸已修正到 `0x128`。
  - 已进一步补出共享 `stage preset` 池：`SceneNode + 0xA0` 为实例级 preset 池起点，`MeshLayerDispatchRecord + 0x0C/+0x10/+0x24/+0x28` 决定 texture stage 模板选择。
  - 已继续细化 prepared-primitive 提交：`MeshData + 0x0C/+0x10/+0x48/+0x4C/+0x58` 已能保守提升为主流参数/主流指针/主流 stride/第二流指针/第二流 stride，`MeshLayerStateRecord + 0x00` 更接近 layer 级 `primary_resource_binding`。
  - 已追到 stage preset builder 上游：`CModel_SetWorldMatrixAndBuildStagePresets -> WithOverrides/Simple -> AssignVisiblePartStagePresetSpan -> CopyResolvedStagePresetsToOutputBuffer`，确认存在 48 字节 preset temp arena 与 4 字节 override map temp arena。
  - 已把 `sub_6F77C260` 收紧成 `RenderOverrideGraph_Evaluate` 四层模型：`runtime controller + immutable body + eval context + output buffers`，并补出了节点骨架里的 `source_vector_index / output_slot_index / node_type / child_array`。
  - 已继续追到 `CModel` 的 part-state controller：确认 `+0x98/+0xA8/+0xAC/+0xC4/+0xC8/+0xD4` 这组字段在动画推进、child bucket 递归、pose 输出与 RenderQueue 入队间共享；`GxDevice_BindPrimaryResource(1/2/3)` 也已收敛到三类内建 primary resource profile，而不再视为普通魔法数。
  - 已将 `RenderablePart + 0x08` 坐实为 `stagePresetSpanBaseIndex`，并继续拆出 `RenderStagePresetOverrideGraph_Execute` 的节点骨架（`transform index / output slot / node type / child array`）。
- 已新增 `21_render_logic_bridge_optimization_notes/README.md`：
  - 盘点了当前仓库里 render-layer 到 logic-layer 的真实桥接链、热路径成本和 dormant 直桥入口。
  - 已确认 `TLS dispatch handle / current batch entry / current world object context` 三条直桥入口都已存在，但生产端尚未真正接线。
- 已新增 `22_cmodel_pose_palette_reverse/README.md`：
  - 已把 `CModelData/CGeosetData` 的 geoset、matrix-group、matrix index、material/layout 绑定收口到同一页，并明确 `vertex_group_indices` 是“每顶点 matrix-group slot 表”，不是直接 bone id。
  - 已确认 `CModel + 0x5C/+0x60` 是本帧最终稳定的 3x4 pose palette；动态单位未来应优先走“静态模型资源 + 每帧 palette 更新”路线，而不是 CPU skin output 路线。
- 已新增 `23_blob_shadow_lista_upstream_reverse/README.md`：
  - 已修正“`RegisterImageEntry = ListA 本体上游`”这一旧结论，确认 `0x713250` 更接近 stamp/image 注册池，而 `ListA` 本体网格由 `0x738ED0 -> 0x73DC00 -> 0x73D9F0` 批量构建。
  - 已确认建筑静态阴影真正的高层生产者是 `ShadowPath_StaticStamp_Toggle / TerrainShadow_ToggleStaticStampFromObject / TerrainShadow_ToggleEmitterStamp / CTerrainUberSplats(WithParams)`，并由 `0x74D500/0x751290/0x759880/0x7599F0/0x75C5F0` 这一对象级调度层统一开关。
- 已新增 `24_cdoodads_static_shadow_upstream/README.md`（v3 重大修正版）：
  - **v1 误判**：把 `CDoodads` 5 个调度器当成"建筑/可破坏物/装饰物的真正治理点"——错。`CDoodads` 只管树木/装饰物/腐地，不管建筑。
  - **v2 误判**：把 CUnit `ShadowProjector` 列表当成"建筑阴影路径"——也错。那是单位脚下方块 emitter，不画建筑底部矩形贴花。
  - **v3 决定性发现**（本轮）：通过 `list_funcs("TerrainShadow")` 发现一组之前所有研究都未提及的函数：
    - `TerrainShadow_WriteMaskRegion (0x234710)` — 真正的写入函数；
    - `TerrainShadow_WriteMaskRegion_ForObject (0x234620)`；
    - `TerrainShadow_WriteMaskRegion_FromActorRuntime (0x3DB260)`；
    - `TerrainShadow_RebuildMaskFromObjectLists (0x233E90)`。
  - 这条路径**完全绕过 ListA/ListB/RegisterImage/Stamp 任何注册池**，直接修改 `CFogMaskTable`（源文件
    `War3\Source\Game\CFogMaskTable.h`）的 16-bit mask grid，由地形渲染管线在画地面 tile 时按 mask bit 着色。
  - 6 个 caller 中包含两个关键中央点：
    - `sub_6F65A140` → `CWidget_RegisterFootprintAndShadowMask`（30+ caller，几乎所有 unit 状态变化）；
    - `sub_6F514F40` → `CUnit_StampBuildingShadowFootprint`（CUnit lifecycle helper）。
  - **反证证据**：AGENTS 第 57 条 `Mode1_BlockAllRegisterImage` 极限实验把 RegisterImage 全部封堵后游戏崩溃，
    但建筑阴影并没消失——证明 RegisterImage 根本不是建筑阴影的写入路径。
  - **正确治理路径**：hook `TerrainShadow_WriteMaskRegion` 本身，按 `a3 type code` 的 16 bit 拆分屏蔽（高 7 bit 是 elevation、低 9 bit 是
    type 标志；具体哪一 bit 是 building shadow footprint 需要下一轮 in-game A/B 实验逐 bit 锁定），
    **绝不能整体 return**（会同时关 fog-of-war / 视野 / 路径阻挡）。
  - CDoodads `a6` mask 注入方案（v1/v2 蓝图）仍然有效，可独立关树木/装饰物/腐地阴影；
    CUnit ShadowProjector hook 只能关单位脚下方块那种 emitter 阴影。
  - 已把 14+8 个关键函数（CDoodads / CUnit / FogMask 三套）命名 + 中文注释回写到 IDA。
- 已基于 ASM 确认 `TerrainShadow_RenderLayer` 的参数语义：`a2` 控制 ListA，`a3` 控制 ListB。
- 已基于 ASM 追加确认：`CWorld_TerrainShadow_Dispatch(stage14)` 会直调 `TerrainShadow_RenderListB(type=4)`，不经过 `RenderLayer(a3)`。
- 已补齐 `ShadowProjector_Add_FromObject / ShadowProjector_Add_Simple` Hook 入口，接入可配置拦截策略。
- 已新增 `TerrainShadow_RenderListB` Hook：mode=1 定向拦截 `type=4`，mode>=2 可全拦截 ListB。
- 方向2（LOSBlocker）已完成结项：`rawcode` + `Sprite->Model` 双通道过滤稳定生效，阻断器阴影已清除。
- 已修复合批 fallback 状态位回归：`SingletonBypass`/尾部 fallback 恢复原始 `layerChanged/stateChanged` 计算，不再强制 `1/1`。
- 已新增 `native` 研究目录，开始按 ASM 固化渲染主链（`CWorld_RenderScene -> DispatchStage -> RenderGroup -> Submit/Dispatch -> Flush`）与阴影分支链路。
- 已新增 `src/d3d9/war3/native/address_book/README.md`，沉淀地址、调用约定、阶段映射和未还原点清单。
- `src/d3d9/war3/native/war3_native_renderer.cpp` 已按 ASM 顺序重排主调度链（含两次 Flush 时机与 group 分发偏移）。
- `DispatchStage case16/18/21` 已按 ASM 补到 native 参考代码，`RenderQueue_StageUpdate(0x6F13A9B0)` 初始化路径与关键全局地址已确认。
- 已回退会误杀贴花的阴影粗拦截策略（Projector mode>=1 全拦截 / ListB type4 默认拦截）。
- 已补齐 LOSBlocker 桥接链路：PathBlocker 强制桥接扩展到 WorldObjects/Selection/Decorations，作为维护态回归保障。
- 已新增静态阴影上游诊断：`ShadowUpdate cbTop` 回调频次统计 + `Projector key sample` 采样。
- 已新增 JASS VM 追踪专项：`ExecuteJassFunctionInternal + JassInterpreter_MainLoop` 双钩子、返回码统计与预算策略开关。
- 已新增渲染热路径“局部合并提交”实验：`Dispatch_Common/Special` 对同 `renderablePart` 的 `ExecBatch Begin/End` 进行上下文复用，并在 `FlushAndReset` 帧尾收口。
- 已完成 `Blob/ListA` 静态阴影链路修正：
  - `TerrainShadow_RegisterImageEntry(0x713250)` 对应的是 `0xA0` 的 stamp/image 注册池，不是 `ListAEntry(0x94)` 主入口。
  - `ListA` 建筑静态阴影的更上游链路是 `ShadowPath_StaticStamp_Toggle(0x74E420) -> ShadowStamp_WriteByName(0x713B20) -> ShadowStamp_WriteCore(0x713920) -> TerrainShadow_MarkListARegionDirtyFromStampRect(0x72FA40)`。
  - `TerrainShadow_ToggleStaticStampFromObject(0x74DB30)` / `TerrainShadow_ToggleEmitterStamp(0x74DE40)` 仍然重要，但更偏 `RegisterImage/ListB-like` 注册池，而不是这轮要处理的 `ListA` 主写入链。
- 已新增主循环深度阶段追踪：`Engine/TlsPump/SelectWorker/RunCallbacks/QueueFlush/FinalizeTick/Reschedule`，用于拆分 `Other/Untracked`。
- 已补齐主循环深层阶段计时：`PrepareWait/PrepareDispatch/FinalizeDispatch/TickUpdate/FinalizeWorker/ComputeWakeDelta`，并将 `Dispatch` 细化到 `Case0~Case14`。
- 已完成第五轮渲染优化落地（保守接管阈值自适应、透明排序快路径、ShadowMap 自适应更新、诊断默认门控）。
- 已完成第六轮“渲染优化 × 逻辑优化”8 组组合矩阵验证；唯一故障项（JASS 深层 Hook）已修复并复测通过。
- 本夜结果已归档：`09_2026_02_24_nightly_closeout/README.md`。
- 已完成 P3 契约化拆分（Dispatch 合同层 + Queue 策略层），并新增静态收益门控文档：`10_p3_contract_static_gate/README.md`。
- 已完成第四轮 MainLoop 未知项压缩：分析模式下 `avgUntrackedActiveCpuMs=0.000`，并新增 `DispatchModule/*` 语义分桶与模块级优化计划（见 `11_mainloop_round4_unknown_resolution/README.md`）。
- 已新增 MainLoop 全量逆向证据包：`12_mainloop_full_reverse/README.md` 与 `ida_mainloop_dump_2026_02_25.json`（IDA MCP 直接导出）。
- 已完成第四轮续作二次收敛：`RunCallbacks/PrepareDispatch/TickUpdate` 改为“每循环批量上报”，并通过 60 秒 AutoTest（`war3_perf_report_auto_2026_02_25_04_45_11.html`，`ok=true`）。
- 已完成第五轮配置矩阵与上限探索：主矩阵最佳 `122.804 FPS`，关闭 mode1 ShadowCapture 可达 `209.268 FPS`（实验档）；默认配置已回正为“画质优先”（保留 ShadowCapture，见 `12_round5_perf_matrix_150fps/README.md`）。
- 已新增 `Storm.dll` 内存钩子 ASM 记录：明确 `SMemFree/GetSize` 会固定读取 `user[-2/-5/-8/-12/-16]`，后续 StormBreaker 路径必须遵守“私有头、拒绝伪装原生块”的约束（见 `16_storm_memory_hook_reverse/README.md`）。

## 留言1~4归纳（速览）
| 留言 | 主题 | 核心产出 | 主要代码落点 | 当前结论 |
|---|---|---|---|---|
| 留言1 | JASS VM 性能研究 | `ExecuteInternal/MainLoop` 追踪、返回码统计、预算策略开关 | `war3_hook_jass.cpp` / `war3_hook_address_book.*` | 已形成可观测链路，可区分 timeout/paused/错误路径 |
| 留言2 | 局部合并提交 | `Dispatch Local Merge`（同 `renderablePart` 复用 Begin/End） | `war3_hook_render.cpp` / `war3_hook_lifecycle.cpp` | 已落地，保持语义不变并降低桥接开销 |
| 留言3 | 非合批优化路线 | `Dispatch Tag/Stage` 线程本地缓存 + 文档化归纳 | `war3_hook_render.cpp` / `war3_internal_test_config.h` | 已落地，热路径重复查询成本下降 |
| 留言4 | 静态阴影根因收敛 | 已确认 `StaticStampPath -> ShadowStamp_WriteCore` 是 `ListA` 主写入链，`RegisterImageEntry` 更偏 `0xA0` 注册池 | `war3_hook_shadow.cpp` / `d3d9_war3_hook.cpp` / `war3_hook_address_book.*` | 当前主方向是“前移到 StaticStampPath + RegisterImage 来源分类”，`ListA` 末端仅保守兜底 |

## IDA MCP 核验结论（2026-02-22）
1. 阴影投影写入主链与文档一致：
   - `sub_6F713CA0` 的 code xref 仅来自
     - `ShadowProjector_Add_Simple(0x6F76D790)`
     - `ShadowProjector_Add_FromObject(0x6F76D800)`
2. `ShadowProjector_Add_FromObject(0x6F76D800)` 先经过 `sub_6F76A490`，再进入 `sub_6F713CA0`。
3. `ShadowProjector_Add_FromObject` 的上游来源确认：
   - `ShadowPath_ObjectProjector_Runtime(0x6F38D7A0)`
   - `ShadowPath_ObjectProjector_JassBridge(0x6F1DEEA0)`
4. `CWorld_TerrainShadow_Dispatch` 的 `stage14` 路径仍存在 `RenderListB(type=4)` 直调，不依赖 `RenderLayer(a3)`。
5. 合批热点链路确认：
   - `RenderQueue_FlushSortedItems(0x6F1380A0)` 每个排序后条目都会走 `Dispatch_*` 与 `RenderQueue_StageUpdate`；
   - `RenderQueue_Dispatch_Common(0x6F13A5E0)` 固定调用 `RenderQueue_UpdateItemWorldMatrix`，说明“减少 dispatch 次数”是首要性能杠杆。

## 统一执行计划（2026-02-22 起）
| 方向 | 现状 | 下一步动作 | 验收指标 |
|---|---|---|---|
| 方向1 合批 | 合批框架已工作，但仍有较高单发与回退成本 | 在 `BatchMergeStats` 基础上新增回退原因分桶（`NoShader/StateMismatch/AppendBreak/...`）并按场景量化 | `FQ_Dispatch_Opaque` 占比下降；`dispatchCommon` 调用数下降 |
| 方向2 LOSBlocker | 已收敛：原版阻断器 FourCC 已稳定从 ShadowCapture 拒绝 | 进入维护：仅保留自定义地图变体抽样与黑名单补充；透明材质描边问题转入独立专项 | 阻断器不再进入阴影，且无“全体描边”回归 |
| 方向3 静态阴影 | 已掌握 `ListB type=4`、`RegisterImage` 与 `StaticStampPath` 三条入口 | 以前移的 `StaticStampPath + RegisterImage` 拦截为主，`ListA` 末端仅保守兜底 | 建筑静态阴影消失且雾/边界/贴花不误伤 |
| 方向4 架构重构 | P1/P2 已落地：主入口瘦身 + 分域接线 + AddressBook | 持续推进统一 Hook 注册器、诊断分级与桥接契约化 | `d3d9_war3_hook.cpp` 持续保持“薄中枢”，分域可独立回归 |
| 方向5 JASS+局部合并提交 | 已完成 JASS 主循环入口定位与 Hook（`0x7F1A20/0x7F2D92`） | 先以“同 renderablePart 局部上下文复用”压低 Dispatch CPU，再决定是否扩大到全队列接管合批 | `Hook_Dispatch_*` CPU 下降，`DispatchLocalMerge reusePct` 稳定上升且画面无回归 |

## 验证总清单
1. 运行 `ninja -C build32`（已通过）。
2. 进游戏压测同场景，记录 FPS 与 `Opt/BatchMerge/*`、`FQ_Dispatch_Opaque`。
3. 重点观察：
   - `ShadowCapture` 次数是否下降；
   - LOSBlocker 是否不再进入阴影（已完成，后续仅抽检）；
   - 建筑静态阴影在 mode=1 下是否被抑制。

## 研究方向结项总览（2026-02-24）
| 方向 | 状态 | 结论 |
|---|---|---|
| 01 合批/批次提交 | 进行中（阶段性） | 已形成“保守接管 + 局部合并 + 透明排序快路径”可用组合，收益稳定；全量接管仍需按场景继续 A/B。 |
| 02 LOSBlocker 阴影 | 已结项 | 已稳定屏蔽阻断器阴影，且保持描边链路可用。 |
| 03 建筑静态阴影 | 进行中（高级阶段） | 已从末端拦截转向上游写入链治理；当前主治理点是 `StaticStampPath`，`RegisterImageEntry` 退回到配套来源分类与兜底角色。 |
| 04 架构重构 | 已完成 v1 | 入口瘦身、分域 Hook、AddressBook 与模块化文档已完成。 |
| 05 JASS VM + 局部提交 | 已完成阶段目标 | 已建立 JASS 追踪与预算策略、局部合并提交实验链路，并纳入自动化回归。 |
| 07/08 MainLoop 与 WaitGate | 进行中（方法论稳定） | 已完成深层拆解与口径澄清，默认采用“性能模式/分析模式”双配置。 |
| 09 本夜收官 | 已结项 | 完成第五轮 + 第六轮闭环，组合兼容问题修复，报告与证据固化。 |
| 11 MainLoop 第四轮未知项压缩 | 已结项 | 主线程活跃段未追踪项压缩到 0ms（分析口径），并产出模块职责/收益/风险三级优化计划。 |
