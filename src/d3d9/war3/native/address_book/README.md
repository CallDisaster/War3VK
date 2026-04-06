# War3 Native ASM 地址与调用约定清单

## 范围
1. 版本：War3 1.27（Game.dll base=0x6F000000）
2. 用途：`src/d3d9/war3/native/` 还原参考基线
3. 原则：仅记录 ASM 已确认信息

## 主链函数
| 函数 | 地址 | 约定 | 关键参数（ASM 语义） |
|---|---:|---|---|
| `CWorld_RenderScene` | `0x6F3681C0` | `__thiscall` | `ECX=this` |
| `CWorld_DispatchStage` | `0x6F363020` | `__thiscall` | `stageId, mode, categoryMask, activeQueue`（栈传） |
| `CWorld_WorldObjects_RenderGroup` | `0x6F368E30` | `__thiscall` | `groupIdx` |
| `WorldObjectEntry_Render` | `0x6F184EE0` | `__thiscall` | `ECX=entry`，若 `entry+0x20!=0` 则 `jmp RenderQueue_AddBatch` |
| `RenderQueue_AddBatch` | `0x6F139190` | `__thiscall` | `ECX=sceneNode` |
| `RenderBatch_Submit` | `0x6F1375C0` | `__thiscall` | `ECX=sceneNode` |
| `RenderQueue_FlushSortedItems` | `0x6F1380A0` | `__cdecl` | 无 |
| `RenderQueue_FlushAndReset` | `0x6F139800` | `__cdecl` | 无 |
| `RenderQueue_Dispatch_Common` | `0x6F13A5E0` | 混合（ECX/EDX+栈） | `layerIdx/stateChanged/layerChanged` |
| `RenderQueue_Dispatch_Special` | `0x6F13A780` | 混合（ECX/EDX+栈） | `layerIdx/stateChanged` |
| `RenderQueue_StageUpdate` | `0x6F13A9B0` | `__cdecl` 风格（ECX 入参） | `arg=0` 增量 stage 更新；`arg=1` 强制全 stage 更新 |

## Dispatch / Layer 辅助函数（本轮补充）
| 函数 | 地址 | 说明 |
|---|---:|---|
| `RenderBatch_CanEnqueueToMainQueue` | `0x6F1387E0` | 依据“第一个可见 layer 的 blend/draw mode”判断 opaque/transparent |
| `RenderQueue_ComposeLayerTintAndAlpha` | `0x6F137BD0` | 组合 SceneNode tint 记录、layer 可见性和 alpha，输出最终 RGBA |
| `RenderQueue_UpdateItemWorldMatrix` | `0x6F13A510` | dispatch 前的矩阵/空间状态更新 |
| `RenderQueue_BindDispatchBlock` | `0x6F13A710` | 绑定 texture stage mode 与 draw/sampler pair，并维护 cleanup pending |
| `RenderQueue_ApplyTextureStageMode` | `0x6F13AC00` | 根据 layer/state 关系切换 texture stage 0/1 |
| `RenderQueue_ApplyDrawStateAndSamplerPair` | `0x6F138EE0` | 从 `MeshData + LayerState + LayerDispatch` 提交 draw/sampler 组合 |
| `RenderQueue_DispatchSpecialBatch` | `0x6F13A4A0` | special 主路径入口 |
| `RenderQueue_DispatchSpecialAlphaBatches` | `0x6F13A830` | special alpha 子批次循环 |
| `RenderQueue_DispatchFallbackMultiPass` | `0x6F13A180` | special 不一致时的多 pass fallback |
| `RenderQueue_IsSpecialBatchStateConsistent` | `0x6F13AC70` | 检查 special 批次是否能复用状态 |

## RenderQueue / SceneNode 细化语义（本轮补充）
1. `RenderQueue_AddBatch(0x6F139190)` 真实顺序：
   - 先 `RenderBatch_Submit(sceneNode)`
   - 若 `sceneNode+0x94 & 0x10`，继续调用四条透明链：
     - `SceneNode_AddTransparentList0`
     - `SceneNode_AddTransparentList2`
     - `SceneNode_AddTransparentList3`
     - `SceneNode_AddTransparentList4`
   - 最后按 `sceneNode+0xC8` 的 child bucket 递归子节点
2. `sceneNode+0x98` 是子节点可见性查询上下文；
   `sceneNode+0xD4` 是缓存字节表（bit0=已缓存，bit1=可见）。
3. `sceneNode+0xC8` 指向 stride=0x0C 的 bucket 数组，bucket +0x08 为首个子链接。
4. 子链接节点布局可按：
   - `+0x04 = next`
   - `+0x08 = childSceneNode`
5. `RenderBatch_Submit(0x6F1375C0)` 的核心条目语义：
   - `part+0x0C = meshData`
   - `part+0x10 != 0` 时直接跳过
   - `part+0x14 = sceneNode`（回写）
   - `meshData+0x104` 命中时，入队条目 `flags.bit0 = 1`
   - 同一 mesh 剩余可见层存在时，入队条目 `flags.bit1 = 1`
   - `(flags & 3) == 3` 时，后续 flush 阶段会走 `Dispatch_Special`
6. `RenderQueue_FlushSortedItems(0x6F1380A0)` 已确认：
   - 先复制 batch 指针到 `g_RenderQueue_SortedPtrs`
   - `qsort`
   - 先对首条 `ApplyStateBlock`
   - 每条之后都会做一次 `RenderQueue_StageUpdate(0)`
   - 尾部若 `g_RenderQueue_StateCleanupPending != 0`，则做 `StateCleanup74/78`
7. `RenderQueue_Dispatch_Common(0x6F13A5E0)` 已确认：
   - 必定先 `RenderQueue_UpdateItemWorldMatrix`
   - 通过 `meshIndex -> dispatch block` 取层描述
   - `RenderQueue_BindDispatchBlock`
   - 按需要 `GxDevice_ApplyStateBlock`
   - 非 special mesh 尾部会补一次 `RenderSceneFlush_0E39E0`
8. `RenderQueue_Dispatch_Special(0x6F13A780)` 已确认：
   - 必定先 `RenderQueue_UpdateItemWorldMatrix`
   - 状态一致时走 `RenderQueue_DispatchSpecialBatch`
   - 状态不一致时会先清理，再走 `RenderQueue_DispatchFallbackMultiPass`

## RenderQueue 二进制真实签名（用于 takeover 设计）
1. `RenderBatch_CanEnqueueToMainQueue`
   - `__fastcall(SceneNode* sceneNode, RenderablePart* part)`
2. `RenderQueue_Dispatch_Common`
   - `__fastcall(SceneNode* sceneNode, RenderablePart* part, int layerIndex, int stateChanged, int layerChanged)`
3. `RenderQueue_Dispatch_Special`
   - `__fastcall(SceneNode* sceneNode, RenderablePart* part, int layerIndex, int layerChanged)`

## 高置信度结构锚点（2026-04-04 追加）
1. `RenderBatchElement`
   - `+0x00 = RenderablePart*`
   - `+0x04 = flags`
   - `+0x08 = layerIndex`
   - `+0x0C = layerCounter`
   - `+0x10 = MeshLayerStateRecord*`
2. `RenderablePart`
   - `+0x0C = MeshData*`
   - `+0x10 = skipFlag`
   - `+0x14 = SceneNode*`
3. `MeshInfo`
   - `+0x0C = layerCount`
   - `+0x10 = MeshLayerStateRecord*`
   - `+0x38 = LayerInfo*`
4. `LayerInfo`
   - `+0x10 = MeshLayerDispatchRecord*`
5. `MeshLayerDispatchRecord`
   - `+0x0C/+0x10 = stage preset index`
   - `+0x14/+0x18 = aux ref index`
   - `+0x1C = visibility offset`
   - `+0x20 = alpha flags`
   - `+0x24/+0x28 = stage mode（>=12 时走共享 preset）`
6. `MeshLayerStateRecord`
   - `+0x00 = primary resource binding`
   - `+0x18 = blend/draw mode`
   - `+0x1C/+0x20 = aux ref enable`
7. `MeshData`
   - `+0x0C = primary stream arg0`
   - `+0x10 = primary stream ptr`
   - `+0x48 = primary stream stride`
   - `+0x4C = stream1 ptr`
   - `+0x58 = stream1 stride`
   - `+0x124 = extra mesh flags`
   - 当前仅确认 `bit2` 会影响 `0x6F138510` 的过滤分支
8. `SceneNode`
   - `+0xA0 = stage preset base index`
   - `+0xC8 = child bucket array`
   - `+0xD4 = child visibility cache`
9. `MeshAuxResourceEntry`
   - `+0x08 = resource_binding`
10. `RenderablePart`
   - `+0x08 = stage preset span base index`
   - `+0x0C = meshData`
   - `+0x10 = skip flag`
10. `SceneNodeChildLink`
   - `+0x04 = next_link`
   - `+0x08 = child_scene`
   - `+0x0C = link_flags(bit0=允许递归传播到 child_scene)`

## Draw Helper 低层角色（2026-04-04 继续补齐）
1. `sub_6F0E35B0`
   - 不是 draw
   - 是一次多槽顶点/属性源绑定提交
   - `RenderQueue_ApplyDrawStateAndSamplerPair`、地形阴影和若干 image-like primitive 都会先走它
   - 当前已能把 13 个参数保守拆成主流参数 + 5 组 `ptr/stride(or width)` 对
2. `sub_6F0E3550`
   - 是真正的底层 draw 调用
   - 调用约定为 `ECX/EDX + 栈`
3. `sub_6F0E3520`
   - 是 `sub_6F0E3550` 的包装器
   - 额外补 `sub_6F0E3540 + GxDevice_StateCleanup74`
4. `sub_6F0E38D0`
   - 更像 `GxDevice_BindPrimaryResource`
   - 世界对象主路径会直接吃 `MeshLayerStateRecord + 0x00`
5. `sub_6F0E38E0`
   - 更像 `GxDevice_ResetSecondaryResource`
6. `sub_6F13ABA0`
   - 负责把 layer tint/alpha 写入两个 color/alpha 槽
   - `alpha_flags.bit0` 置位时会走交换路径

## Shared Stage Preset 线索（2026-04-04 继续补齐）
1. `sub_6F138FF0`
   - 行为更像共享 stage preset 池的 span 分配器
2. `sub_6F139060`
   - 返回 `base + 48 * index`
   - 已确认 stride 为 `0x30`
   - 记录本体会参与 3x4 变换矩阵乘法、缩放和点变换
3. `RenderQueue_UpdateTextureStageSlot`
   - 读取的是 `MeshLayerDispatchRecord + 0x0C/+0x10/+0x24/+0x28`
   - 若 `stage_mode >= 12`，则以 `SceneNode + 0xA0` 为起点在共享 preset 池中取记录
4. `CModel_SetWorldMatrixAndBuildStagePresets(0x6F12F0A0)`
   - 设置模型当前 3x4 变换并 push scratch preset
   - 按 flags.bit4 分流到 `WithOverrides` / `Simple`
5. `CModel_BuildVisiblePartStagePresets_WithOverrides(0x6F12E900)`
   - 申请 48 字节 preset temp buffer + 4 字节 override map temp buffer
   - 经 `sub_6F77C260` 展开 override graph
6. `CModel_AssignVisiblePartStagePresetSpan(0x6F12FED0)`
   - 给可见部件分配连续 preset span，并把 span 起始 index 写到部件记录 `+0x08`
7. `CModel_CopyResolvedStagePresetsToOutputBuffer(0x6F12FDC0)`
   - 把临时缓冲里的 preset 拷回模型输出缓冲
8. `CModel + 0x98 / +0xC4 / +0xC8 / +0xD4`
   - `+0x98 = part state / override graph controller`
   - `+0xC4/+0xC8/+0xD4 = child bucket count / child bucket array / cached state byte array`
   - `+0xC8` 当前看复用了 `SceneNodeChildBucket` 同布局
9. `sub_6F777FE0`
   - 更像 `CModelPartStateController_IsPartEligibleForCurrentState`
   - 会同时检查 stride `228` 的 state 表和 stride `56` 的 weight/visibility 表
   - 被 stage preset 构建、动画推进和 RenderQueue 入队共用
10. `sub_6F77C260`
   - 当前可保守理解为 `RenderOverrideGraph_Evaluate`
   - 可以拆成：`runtime controller + immutable body + eval context + output buffers`
   - 节点骨架当前已确认：
     - `+0xA8 = source_vector_index`
     - `+0xAC = output_slot_index`
     - `+0xB0 = node_type`
     - `+0xB1 = node_flag_bits`
     - `+0xB8 = child_count`
     - `+0xBC = child_array`
     - `+0xC4 = visibility_dependency_index`

## GxDevice 主资源 profile 线索（2026-04-04 继续补齐）
1. `GxDevice_BindPrimaryResource(1)`
   - 代表路径：`CWorldFrameWar3_RenderDebugOverlayDispatcher(0x6F368ADB)`、`CWorldFrameWar3_RenderOverlayGridAndCells(0x6F36958B)`
   - 共同特征：简化 state block、调用者不显式绑常规 uv/texture 流、后续经 debug line/cell helper 逐段提交
   - 当前更像 `debug immediate line/cell profile`
2. `GxDevice_BindPrimaryResource(2)`
   - 代表路径：`TerrainShadow_ListA_RenderSubBatch(0x6F736C39)`、`TerrainShadow_RenderListA(0x6F73752E)`、`TerrainShadow_Node7E4_Render(0x6F6F4D03)`
   - 共同特征：先 `sub_6F704950(...,2)` 选材质，再经 `sub_6F705090/0x6F705120` 做拓扑适配后 draw
   - 当前更像 `projected shadow mesh / topology-adaptive profile`
3. `GxDevice_BindPrimaryResource(3)`
   - 代表路径：`RenderImageLikePrimitiveBatch(0x6F19E0BF)`、`TerrainShadow_RenderListBEntry(0x6F737325)`、`sub_6F19BC20`
   - 共同特征：后续通常直接接 `BindMultiSlotResourceSet` 或 `sub_6F0E3580`，并吃 `12+8` 分离流或 `20B(pos+uv)` 单流
   - 当前更像 `generic textured prepared-primitive profile`
4. `sub_6F0E3580(0x6F0E3580)`
   - 当前更像 `PrepareDynamicPreparedPrimitive_Profile3`
   - 代表现场：`RenderImageLikePrimitiveBatch(0x6F19E180)`
   - 高置信度参数骨架：
     - `ECX = totalVertexCount`
     - `EDX = 3(profile3)`
     - `stack arg0 = totalIndexCount`
     - `stack arg4/arg8 = caller 上两个 reservation/output slot`
   - 不是普通 bind helper，而是 profile3 动态 scratch / prepared primitive 的预备器
5. `sub_6F0E35B0(0x6F0E35B0)`
   - 当前更像 `BindPreparedPrimitiveStreams`
   - 高置信度参数骨架：
     - `arg0 = slot0_header`
     - `arg1 = slot0_ptr`
     - `arg2 = slot0_stride`
     - `arg3/4 = slot1_ptr/slot1_aux`
     - `arg5/6 = slot2_ptr/slot2_aux`
     - `arg7/8 = slot3_ptr/slot3_aux`
     - `arg9/10 = slot4_ptr/slot4_aux`
     - `arg11/12 = slot5_ptr/slot5_aux`
   - 代码中会在 `slotN_ptr==0` 时自动清零对应 `slotN_aux`
   - 更像多路 vertex/aux attribute source 绑定，不像 texture/state object 绑定
6. `sub_6F0E3550 / sub_6F0E3520(0x6F0E3550 / 0x6F0E3520)`
   - 当前更像 `DrawPreparedPrimitive / DrawPreparedPrimitiveWithCleanup`
   - 高置信度参数骨架：
     - `ECX = primitiveType/topology id`
     - `EDX = preparedCount`
     - `stack arg0 = prepared primitive backing`
   - `0x6F0E3520` 只是在保留 `ECX/EDX` 的前提下补 `sub_6F0E3540 + StateCleanup74`
7. `sub_6F0C0260(0x6F0C0260)`
   - profile3 的“CPU 临时四边形 + 静态 prepared index 模板”样板
   - `unk_6FB66500`：
     - 只被这里拿来当 `DrawPreparedPrimitiveWithCleanup` 的第三实参
     - 开头 8 字节就是 `u16 {0,1,2,3}`
     - 当前更像静态 quad/strip prepared index 模板
   - `unk_6FB66508`：
     - 只被这里当 `BindPreparedPrimitiveStreams` 的 `slot1_ptr`
     - 当前更像共享 sideband attribute block，不像 texture/state object
8. 当前高置信度：
   - `1/2/3` 不是普通纹理 handle
   - 更像 GxDevice 内建的几组 primary resource / binding contract
9. 目前已经抓到的代表现场：
   - `1 -> CWorldFrameWar3_RenderOverlayGridAndCells`
   - `2 -> TerrainShadow_ListA_RenderSubBatch`, `sub_6F133F00`
   - `3 -> RenderImageLikePrimitiveBatch`, `TerrainShadow_RenderListBEntry`, `sub_6F0C0260`
10. `RenderQueue_UpdateItemWorldMatrix(0x6F13A510)`
   - 直接读取 `renderablePart + 0x08`
   - 说明 builder 写入的 span 起始 index 会在 dispatch 阶段被直接消费
11. `sub_6F77C260`
   - 当前可保守理解为 `RenderStagePresetOverrideGraph_Execute`
   - 其节点对象当前能确认：
     - `+0xA8 = transform index`
     - `+0xAC = output slot / part index`
     - `+0xB0 = node type`
     - `+0xB1 = mode bits`
     - `+0xB8 = child count`
     - `+0xBC = child array`
   - 节点类型目前还能细化：
     - `0 = no-op`
     - `1 = 更新 CGxuLight 指针输出`
     - `2 = gate resolve 后直接写 48B preset 输出`
     - `4 = 更新 0x80 ParticleEmitterOverrideSlot -> 0x68 CParticleEmitterRuntime`
     - `5 = 更新 0x8C PlaneParticleEmitterOverrideSlot -> CPlaneParticleEmitter*`
     - `6 = 更新 0x74 CAnimRibbonObjStatus -> 0x16C CAnimRibbonObj`
     - `7 = 更新 0x40 stride 的局部点位/向量输出`
     - `default = 直接把当前 48B preset 拷回输出缓冲`
8. `sub_6F12EE90 / CModel_SetAnimationTimeMs / sub_6F12FAA0`
   - 三条递归链都复用了 `SceneNodeChildBucket(first_link@+8)` 和 `SceneNodeChildLink(next@+4, child@+8, flags@+0x0C)` 这套子节点结构

## Override 输出 slot 与真实对象（2026-04-04 新补齐）
1. node_type=4 / `sub_6F77EEB0`
   - override slot：`runtime + 148 + 0x80 * slot`
   - 真正对象：`a1+48 -> +0x18 -> +0x08 + 0x68 * slot`
   - 当前可保守命名：
     - slot = `ParticleEmitterOverrideSlot`
     - 对象 = `CParticleEmitterRuntime`
   - `sub_6F1991A0` 会维护 active/free index 栈并逐个推进 `0x28` 粒子实例
2. node_type=5 / `sub_6F77EC10`
   - override slot：`runtime + 156 + 0x8C * slot`
   - 启停对象：`a1+48 -> +0x1C -> +0x08[index]`
   - 矩阵对象：`*(slot + 0x88)`
   - 分配标签命中 `AVCPlaneParticleEmitter`
   - 当前可保守命名：
     - slot = `PlaneParticleEmitterOverrideSlot`
     - 对象 = `CPlaneParticleEmitter`
3. node_type=6 / `sub_6F77F9D0`
   - status：`runtime + 164 + 0x74 * slot`
   - 真正对象：`a1+48 -> +0x20 -> +0x08 + 0x16C * slot`
   - `status+0x70`：`CAnimRibbonObj*`
   - RTTI 命中：
     - `.?AUCAnimRibbonObj@@`
     - `.?AUCAnimRibbonObjStatus@@`
   - 当前可保守命名：
     - status = `CAnimRibbonObjStatus`
     - 对象 = `CAnimRibbonObj`
     - `status+0x70` 已可按 `CAnimRibbonObj*` 落地
4. 纠偏结论：
   - 之前把 `104/140/116` 直接理解成“对象大小”是不够稳的
   - 更准确的说法是：
     - `0x80/0x8C/0x74` 是 override slot 记录
     - `0x68/ptr/0x16C` 才是后续真正被驱动的对象/对象数组
5. 对象级补充：
   - `CParticleEmitterRuntime_UpdateAndRender(0x6F1991A0)`
     - 直接驱动 `0x68` 对象，内部维护 active/free index 栈和 `0x28` 粒子实例数组
   - `CPlaneParticleEmitterManager_AllocEmitter(0x6F199940)`
     - 分配标签命中 `AVCPlaneParticleEmitter`
     - `CPlaneParticleEmitter_SetTransform(0x6F19F390)` 会把矩阵写到对象 `+0x198`
   - `CAnimRibbonObj_InitFromRibbonEmitterData(0x6F19B0E0)`
     - 只被 ribbon 初始化链命中，负责把上游 ribbon 定义数据装进真正的 `0x16C CAnimRibbonObj`
   - `CAnimRibbonObj_AdvanceAndUpdateVertices(0x6F19C520)`
     - 明确依赖 `GetTickCount`
     - 负责 segment/UV/几何条带推进，而不是简单 enable toggle
   - `sub_6F0CC450`
     - 分配 `44` 字节 `CGxuLight`
     - 被 `COmniLight_SyncGxuLight` 和 node_type=1 共用
6. `node_type 1/2/7` 还能继续保守落名：
   - `ctx->outputs + 0x14 = gxuLightArrayHandle`
   - `ctx->outputs + 0x24 = sharedPresetOutputs`
   - `runtime + 0xB4 = RenderOverrideLocalPointOutputRecord[]`
   - 其中 `RenderOverrideLocalPointOutputRecord + 0x34 = resolvedLocalPoint`
7. `ctx->outputs` 现在已经能保守拆成一张部分结构：
   - `+0x08 = primaryPresetOutputs`
   - `+0x18 = particleEmitterRuntimeArrayHandle`
   - `+0x2C = dependencyGateOutputs`
   - `+0x38 = visibilityByteOutputsHandle`
   - `+0x3C = compactScalarOutputs`
8. `sub_6F12E900 / sub_6F12EB70`
   - 说明 `sub_6F77C260` 的 caller 会先组一块 `16 dword` builder 参数块
   - 这块参数同时打包了：
     - source point/vector 集合
     - preset 输出表
     - 粒子/缎带 slot 数组
     - visibility cache

## CModelData / Pose Palette 关键锚点（2026-04-04 新补齐）
1. `0x6F127610`
   - `CModelData_CreateOwnedHandle`
   - 分配 `HMODELDATA`，调 `CModelData::ctor`，把结果写到调用者 `+0x9C`
2. `0x6F12A400`
   - `CModel_CreateWithOwnedModelData`
   - 分配 `HMODEL` + `HMODELDATA`
   - `CModelData + 0x54 = 1`
   - `CModel + 0x9C = retain(modelData)`
3. `0x6F126250`
   - `CGeoset_CreateFromRawArrays`
   - 已确认：
     - `CGeoset + 0x0C = CGeosetData*`
     - `CGeosetData + 0x08 = positions`
     - `CGeosetData + 0x44/+0x48/+0x4C = vertex_group_indices`（每顶点 1B matrix-group slot）
     - `CGeosetData + 0x50 = normals`
     - `CGeosetData + 0x8C = uv layer record array`
     - `CGeosetData + 0xC4 = primitive record array`
     - `CGeosetData + 0xD8 = uint16 index buffer`
     - `CGeosetData + 0xF0/+0xF4 = matrix group count / size array`
     - `CGeosetData + 0xFC/+0x100 = total matrix index count / matrix index array`
4. `0x6F131320`
   - `CGeosetData_MergeVerticesAndFillVertexGroupSlots`
   - 会把 source geoset 的 vertex-group remap 结果写到 `CGeosetData + 0x44/+0x48/+0x4C`
   - 同时回写 `CGeosetData + 0x11C = merged_geoset_slot`
5. `0x6F131150 -> 0x6F131210 -> 0x6F132A10`
   - `CGeosetData_MergeMatrixGroupsAndBuildRemap`
   - 会生成 `source group -> remapped slot` 的 byte 映射，并把 dedup 后的 `matrix_group_sizes / matrix_indices` 写到目标 geoset
6. `0x6F12E600`
   - `CModel_ResolveMatrixGroupToPosePalette`
   - 若 group count 为 0 就直接拷当前 3x4；否则按 `matrix_group_sizes + matrix_indices` 组装输出 palette
7. `0x6F12FDC0`
   - `CModel_CopyResolvedPoseMatricesToOutputPalette`
   - 已确认：
     - `CModel + 0x5C = final pose matrix count`
     - `CModel + 0x60 = final pose matrix array`
     - stride = `0x30`
8. 推荐 hook 点：
   - 只要 root runtimeModel 的最终 palette：`post 0x6F12F0A0`
   - 若要连 child runtime / attachment 一起稳定：`post 0x6F12F7E0`
9. 当前最稳结论：
   - 动态单位应优先走 `静态模型资源 + 每帧 3x4 pose/palette` 路线
   - 目前没有比 `CModel + 0x60` 更稳定、也更权威的 CPU skin 输出缓存证据
10. `0x6F131F60`
   - `CModelComplex_BuildChildRuntimeModelLinks`
   - 会把资源侧 child group 克隆成 runtime 侧 16B link 节点
   - `link + 0x08 = child runtimeModel`
   - `link + 0x0C = attach point / child tag`
11. `0x6F12EC90`
   - `CModel_PropagateLocalPointOutputsAndChildRuntimeModels`
   - 先用 part-state controller 产出 attach/local-point 结果，再沿 child runtime tree 递归传播
12. 更上游身份锚点：
   - `0x6F185250 = CSprite_CreateRuntimeModelFromSource`
   - `0x6F6BD110 = SpriteHost_CreateSpriteAndBindSourceObject`
   - 更适合做 `source object -> sprite -> runtimeModel -> rawcode/jHandle` 的前推登记；`sceneNode` 仍建议在 `WorldObjectEntry_Render` 一层补齐

## 阴影链关键函数
| 函数 | 地址 | 说明 |
|---|---:|---|
| `CWorld_TerrainShadow_Dispatch` | `0x6F76F060` | 阶段码总分发 |
| `TerrainShadow_RenderLayer` | `0x6F737620` | `a2` 控 ListA，`a3` 控 ListB |
| `TerrainShadow_RenderListA` | `0x6F737500` | ListA 主渲染 |
| `TerrainShadow_RenderListB` | `0x6F737400` | 按 `type/pass` 过滤后渲染 |
| `ShadowProjector_Add_Simple` | `0x6F76D790` | 对象投影入口 |
| `ShadowProjector_Add_FromObject` | `0x6F76D800` | 对象投影入口 |
| `ShadowUpdate_WriteEntry` | `0x6F73F7A0` | 阴影网格写入路径 |

## `CWorld_RenderScene` 阶段顺序（ASM）
1. 前置：`StateCleanup(world+338/+33C/+354)`，并将 `world+660/+664=-1`
2. 若 `world+300 != -1`，调用 `CWorld_SetShadowMode(1)`
3. 若 `world+358 && world+354 && world+35C`，调用 `DispatchStage(0,0,1,0)`
4. `DispatchStage(1,1,2,activeQueue)`
5. `DispatchStage(13,1,2,activeQueue)`
6. `RenderQueue_FlushAndReset()`
7. `DispatchStage(19,1,2,activeQueue)`
8. `DispatchStage(9,1,2,activeQueue)`
9. `DispatchStage(2,1,2,activeQueue)`
10. `DispatchStage(3,1,2,activeQueue)`
11. `DispatchStage(8,1,2,activeQueue)`
12. 若 `world+324 != 0`，`DispatchStage(17,1,2,activeQueue)`
13. `DispatchStage(14,2,4,activeQueue)`
14. `DispatchStage(5,2,4,activeQueue)`
15. `DispatchStage(10,2,4,activeQueue)`
16. 若 `world+300 != -1`：`sub_6F3621E0(1)` -> `DispatchStage(12,2,4,activeQueue)`
17. `DispatchStage(11,2,4,activeQueue)`
18. `RenderQueue_FlushAndReset()`
19. 若 `world+300 != -1`：`sub_6F3621E0(0)`
20. `DispatchStage(4,1,2,activeQueue)`
21. `DispatchStage(7,1,2,activeQueue)`
22. `DispatchStage(6,1,2,activeQueue)`
23. `DispatchStage(20,2,4,activeQueue)`
24. 若 `activeQueue==0`：追加 `DispatchStage(15,-1,-1,0)`, `DispatchStage(18,2,4,0)`, `DispatchStage(21,-1,-1,0)`
25. 末尾清理：按 `world+664`/`world+660` 关闭状态机，再 flush cleanup context

## `CWorld_DispatchStage` 阶段映射（ASM）
1. `0 -> sub_6F186300(world+354)`
2. `1 -> TerrainShadowDispatch(0)`
3. `2 -> TerrainShadowDispatch(1)`
4. `3 -> TerrainShadowDispatch(2)`
5. `4 -> TerrainShadowDispatch(3)`
6. `5 -> TerrainShadowDispatch(5)`
7. `6 -> TerrainShadowDispatch(8)`
8. `7 -> TerrainShadowDispatch(9)`
9. `8 -> TerrainShadowDispatch(10)`
10. `9 -> TerrainShadowDispatch(6)`
11. `10 -> TerrainShadowDispatch(4)`
12. `11 -> TerrainShadowDispatch(12) + RenderGroup(0)`
13. `12 -> RenderGroup(1)`
14. `13 -> RenderGroup(2)`
15. `14 -> TerrainShadowDispatch(7)`
16. `15 -> sub_6F367980`
17. `16 -> dword_6FB66E24 bit 分支 + sub_6F368A90/sub_6F369560`
18. `17 -> TerrainShadowDispatch(11)`
19. `18 -> sub_6F3597C0/sub_6F3C4330/sub_6F3ACFF0`
20. `19 -> TerrainShadowDispatch(14)`
21. `20 -> TerrainShadowDispatch(15)`
22. `21 -> TerrainShadowDispatch(13) + 阴影/UI 额外分支`

## 当前未还原点
1. `DispatchStage case16/case21` 已内聚到 `native` 桩（RVA + 全局地址），但仍需实机验证版本稳定性。
2. `RenderQueue_StageUpdate(0x6F13A9B0)` 的“stage 描述结构体”字段语义仍待补全。
3. `RenderQueue_AddBatch` 递归与透明子链仅在文档中对齐，代码未完全映射。
4. `Dispatch_Common/Special` 的 dispatch block / bind block / fallback multipass 仍缺完整结构体字段。

## 本轮写回 IDA 的关键函数（2026-04-04）
1. `RenderQueue_AddBatch`
2. `RenderBatch_Submit`
3. `RenderQueue_FlushSortedItems`
4. `RenderQueue_Dispatch_Common`
5. `RenderQueue_Dispatch_Special`
6. `RenderQueue_StageUpdate`
7. `CModel_ApplyWorldMatrixAndDispatch`
8. `CModel_UpdatePoseCacheFromAnimation`
9. `CModel_SetAnimationTimeMs`
10. `CModel_AdvanceAnimationByGameTime`
11. `CWorldFrameWar3_RenderDebugOverlayDispatcher`
12. `CWorldFrameWar3_RenderOverlayGridAndCells`
13. `CWorldFrameWar3_ShouldRenderPostprocessPreview`
14. `CWorldFrameWar3_RenderPostprocessPreviewContext`
15. `CWorldFrameWar3_RenderQueuedUi3DOverlays`
16. `CWorldFrameWar3_RenderStage21Tail`
17. `TerrainShadow_RenderListBEntryChain`

## 本轮已落地代码
1. `war3_native_renderer.h` 的 `RenderStage` / `WorldGroupIndex` / `CWorldFrameWar3`
   已按本页地址表同步。
2. `war3_native_renderer.cpp` 的 `RenderScene` / `DispatchStage` / `WorldObjects_RenderGroup`
   已改成和本页一致的阶段顺序与列表布局。
3. `war3_native_hooks.h/.cpp` 已把 `0x0A1400/0x0A2800/0x363350/0x76F550/0x3621E0`
   等地址改成真实语义函数名，便于后续继续补链。

## StageUpdate 相关全局（已确认）
1. `g_RenderQueue_StageInitialized @ 0x6FBDA4D8`
2. `g_RenderQueue_StageCount @ 0x6FBDA4E0`
3. `g_RenderQueue_StageCountInit @ 0x6FBDA4E4`
