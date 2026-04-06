# Native 渲染链还原（ASM）

## 目标
1. 用 ASM（非伪 C）还原 Warcraft III 原生渲染链。
2. 将 `src/d3d9/war3/native/` 中“占位/推测逻辑”逐步替换为可验证链路。
3. 形成“可实测”的优化切入点，而不是只做概念讨论。

## 本轮已完成（2026-02-21）
1. 主链函数再次用 ASM 复核：
   - `CWorld_RenderScene(0x6F3681C0)`
   - `CWorld_DispatchStage(0x6F363020)`
   - `CWorld_WorldObjects_RenderGroup(0x6F368E30)`
   - `WorldObjectEntry_Render(0x6F184EE0)`
   - `RenderQueue_AddBatch(0x6F139190)`
   - `RenderBatch_Submit(0x6F1375C0)`
   - `RenderQueue_FlushSortedItems(0x6F1380A0)`
   - `RenderQueue_FlushAndReset(0x6F139800)`
   - `RenderQueue_Dispatch_Common/Special(0x6F13A5E0/0x6F13A780)`
2. `src/d3d9/war3/native/war3_native_renderer.cpp` 已改为 ASM 对照主链：
   - 两次 `FlushAndReset` 的位置与时序对齐；
   - `DispatchStage` 顺序按真实阶段表重排；
   - `DispatchStage case16/18/21` 已补为真实 ASM 调用链（RVA + 全局地址）；
   - `WorldObjects_RenderGroup` 采用真实 group 偏移（`+0x16C/+0x170/+0x174`）与 entry stride（`0x18`）。
3. 新增 native 地址与约定清单：
   - `src/d3d9/war3/native/address_book/README.md`
4. 新增原生引擎全景总览：
   - `docs/research/war3_render_issues/19_blizzard_native_rendering_engine_full_perspective/README.md`
5. `native` 构建接线已补齐：
   - `war3_native_renderer_core.cpp`
   - `war3_native_symbols.cpp`
   已纳入 `src/d3d9/war3/native/meson.build`

## 已确认主链（1.27，Game.dll base=0x6F000000）
1. `CWorld_RenderScene`：`0x6F3681C0`
2. `CWorld_DispatchStage`：`0x6F363020`
3. `CWorld_WorldObjects_RenderGroup`：`0x6F368E30`
4. `RenderQueue_Dispatch_Common`：`0x6F13A5E0`
5. `RenderQueue_Dispatch_Special`：`0x6F13A780`
6. `RenderQueue_FlushSortedItems`：`0x6F1380A0`
7. `RenderQueue_FlushAndReset`：`0x6F139800`

## RenderScene 阶段调度（ASM 已固化）
1. 前置：`StateCleanup(338/33C/354)` + 清空 `world+660/+664`
2. 第一段：`0(条件),1,13` -> `FlushAndReset`
3. 第二段：`19,9,2,3,8,(17条件),14,5,10,(12条件),11` -> `FlushAndReset`
4. 第三段：`4,7,6,20`
5. `activeQueue==0` 追加：`15,18,21`

## Stage->阴影分支映射（已确认）
1. `DispatchStage case1/2/3`：进入 `TerrainShadow_RenderLayer`（`ListA/ListB` 由参数控制）
2. `DispatchStage case9/10/11/16`：进入 `TerrainShadow_RenderListB(type=1/2/3/5)`
3. `DispatchStage case14`：进入 `CWorld_TerrainShadow_Dispatch(stage14)`，内部直调 `TerrainShadow_RenderListB(type=4)`

## 阴影链路（ASM 重点）
1. `TerrainShadow_RenderLayer`：`0x6F737620`
2. `TerrainShadow_RenderListA`：`0x6F737500`
3. `TerrainShadow_RenderListB`：`0x6F737400`
4. `CWorld_TerrainShadow_Dispatch`：`0x6F76F060`
5. `ShadowUpdate_WriteEntry`：`0x6F73F7A0`
6. `ShadowProjector_Add_Simple`：`0x6F76D790`
7. `ShadowProjector_Add_FromObject`：`0x6F76D800`

## 当前还原状态
1. `war3_native_renderer.cpp`：
   - 状态：主调度链与 `case16/18/21` 已按 ASM 落地到代码。
   - 未完成：需要实机验证全局地址路径在当前版本是否稳定。
2. `war3_native_renderer_core.cpp`：
   - 状态：`RenderBatch_Submit` 与透明分流流程已定位地址，但代码仍偏“语义版”。
   - 未完成：`RenderQueue_StageUpdate` 与 `Dispatch_*` 细节替换。
3. `war3_native_shadow.cpp`：
   - 状态：链路地址准确，部分字段语义仍在细化。
   - 未完成：`stage -> ListB(type/pass)` 的全表驱动与结构体字段收敛。

## 当前全景判断（2026-04-04）
1. 从“工程接管视角”看，原生渲染主链已经足够清晰：
   - 世界帧调度
   - 阴影阶段分发
   - WorldObjects 入队
   - RenderQueue 排序/派发/flush
2. 从“完整还原暴雪原生引擎视角”看，仍有三层值得继续挖：
   - `dispatch block / special batch / multipass fallback` 的完整结构
   - `CSpriteMini / 骨骼挂点表 / 粒子/缎带` 的类级结构
   - `UI 3D`（portrait/cursor/model frame）的专题化收口
3. 因此本目录现在应被视为“主干已清晰、枝叶仍可继续扩展”的 ASM 基线，而不是“已经无剩余价值”的终稿。
4. 本轮已将 `RenderQueue_*`、`CModel_*`、`stage16/18/21` 尾链函数名与中文作用注释写回 IDA，可直接在符号级继续专题化。

## 与性能优化直接相关的观察（基于 ASM）
1. `RenderQueue_FlushSortedItems` 每 item 都执行 `StageUpdate(0)`，并在主路径反复做状态比较，意味着“减少 item 数量”仍是第一优先级。
2. `RenderQueue_Dispatch_Common` 的开头固定调用 `RenderQueue_UpdateItemWorldMatrix`，如果无法减少调用次数，CPU 开销很难显著下降。
3. `RenderQueue_AddBatch -> RenderBatch_Submit` 在 `sceneNode` 子节点递归时会继续扩展批次来源，解释了大场景下 `ShadowCapture` 与 dispatch 压力放大的原因。

## RenderQueue_StageUpdate（0x6F13A9B0）新结论
1. 首次调用时会通过 `sub_6F0E2DA0` 拉取 stage 描述块并初始化：
   - `g_RenderQueue_StageCountInit @ 0x6FBDA4E4`
   - `g_RenderQueue_StageCount @ 0x6FBDA4E0`
   - `g_RenderQueue_StageInitialized @ 0x6FBDA4D8`
2. 当入参为 `0` 时，只对“未初始化的 stage slot”做 `GxDevice_UpdateStage`；入参非 `0` 时会强制刷新所有 stage slot。
3. 这解释了为什么在 flush 循环中保留 `StageUpdate(0)` 可以减少多余状态提交，但也会放大“批次数过多”带来的循环开销。

## 下一阶段任务
1. 用 ASM 把 `RenderQueue_StageUpdate(0x6F13A9B0)` 参数语义补齐到 native 代码注释。
2. 把 `RenderBatch_Submit(0x6F1375C0)` 的透明四分支（list0/2/3/4）写成表格并落到 native 文档。
3. 为 `case16/case21` 建立单独“全局依赖地址清单”，避免后续继续硬编码猜测字段。
4. 对 `case16/case21` 补充低频日志（仅 native 参考分支），用于验证调用是否命中。
