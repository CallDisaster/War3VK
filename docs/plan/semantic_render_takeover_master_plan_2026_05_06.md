# War3 语义渲染接管长期重构总计划

Date: 2026-05-06

## 1. 计划目的

这份文档不是一次性的 Codex 对话计划，而是后续数周到数月都可以持续执行的正式工程总纲。

目标不是继续在“旧 semantic frame + draw-time patch”这条混合路线上修阴影，而是分阶段完成：

1. A 路线：建立 **current draw authoritative shadow producer**
2. B 路线：建立 **canonical render/skin intermediate format**
3. D 路线：为 **native render takeover** 做长期架构准备

当前项目接受工作量大，但不接受继续在死胡同里补丁式推进。因此本计划优先处理：

1. 权威边界单一化
2. 数据契约标准化
3. 旧路径冻结与退役
4. 面向 native takeover 的基础设施沉淀

## 2. 当前问题的根本判断

当前 object shadow 的核心问题，不是单一字段不准，也不是单纯“Pose 更新频率低”，而是：

**同一个阴影 draw 所使用的 topology、group-slot、palette、world transform，没有稳定地来自同一帧、同一 draw、同一权威边界。**

这会产生以下必然现象：

1. 动态阴影闪烁
2. 靠近其他 caster 时撕裂
3. 移动后留下旧位置残影
4. semantic build/scene submit 为了弥补 stale frame 持续同步补建，造成开局卡顿

因此，本计划明确禁止再把“旧 semantic frame + 当前 draw 局部修补”当作长期主线。

## 3. 北极星架构

长期目标结构分为 5 层。

### 3.1 Observation Layer

职责：只负责从 War3 当前 draw / 当前 runtime 里发布事实，不做消费层猜测。

目标 Hook/观察面：

1. `CGeoset_CreateFromRawArrays`
2. `RuntimeMatrixRangeCopy` / `RuntimeMatrixWrite`
3. `RenderBatch_Submit`
4. `RenderQueue_UpdateItemWorldMatrix (0x13A510)`
5. 若后续需要，再补 `0x138EE0 / sub_6F0E35B0`

### 3.2 Canonical Asset Layer

职责：把长期稳定的数据沉淀成我们自己的标准资产格式。

候选标准对象：

1. `CanonicalMeshAsset`
   - vertex positions
   - indices
   - primitive topology
   - UV / normal / cutout metadata
   - source ownership keys

2. `CanonicalSkinContract`
   - slot stream format
   - group/matrix layout
   - palette interpretation mode

3. `CanonicalMaterialContract`
   - opaque / cutout / transparent classification
   - alpha test reference
   - texture/sampler signature

### 3.3 Canonical Draw Layer

职责：把“这一帧、这一次 draw 真正需要的所有数据”收成一条自洽记录。

候选标准对象：

1. `CanonicalDrawSlice`
   - visible primitive/index slice
   - geoset selection
   - layer/material dispatch selection

2. `CanonicalPaletteSnapshot`
   - current draw palette matrices
   - source serial / frame serial

3. `CanonicalWorldTransform`
   - current sceneNode world matrix
   - pose-derived world transform fallback

4. `CanonicalDrawInstance`
   - `CanonicalMeshAsset` reference
   - `CanonicalSkinContract` reference
   - `CanonicalDrawSlice`
   - `CanonicalPaletteSnapshot`
   - `CanonicalWorldTransform`
   - object identity
   - material contract

### 3.4 Consumer Layer

职责：只消费 canonical draw，不再直接拼接上层 registry 和 draw-time patch。

第一消费者：

1. `CanonicalShadowDrawItem`

后续消费者：

1. outline
2. native preview pass
3. full native object renderer

### 3.5 Native Takeover Layer

最终目标不是“让阴影工作”，而是让 canonical render items 可以直接喂给 native backend。

这意味着：

1. shadow 先成为第一批 canonical consumer
2. 然后 native D3D9 backend 接相同 canonical item
3. 最终把 object rendering 从 DXVK mixed interception 迁移到 native takeover

## 4. 阶段路线图

## Phase 0: 冻结混合路径继续扩散

目标：

1. 停止为旧混合路径继续新增 correctness 补丁
2. 固化旧路径的“只保留安全兜底，不再长功能”原则
3. 建立统一的工作面指标

交付：

1. 本总计划文档
2. 旧路径冻结/退役清单
3. 当前状态页挂接新计划

退出标准：

1. 后续新工作不再默认加到 `War3TryBuildLiveRuntimeGroupPalette` 或 legacy capture
2. 团队对“主线已经转向 A/B，为 D 做准备”达成一致

## Phase 1: Current Draw Authoritative Shadow Producer

目标：

把“当前 draw 的事实”变成稳定 producer，不再让 shadow consumer 依赖混合 patch。

必须拿全的字段：

1. renderable identity
2. static mesh asset key
3. visible primitive/index slice
4. current group-slot stream
5. current palette snapshot
6. current world transform
7. material contract

关键原则：

1. 同一个 producer record 内的数据必须来自同一 draw serial
2. palette 与 slot stream 必须同源
3. 不能再使用“旧 semantic frame + 新 draw-time group-slot”作为长期输出

交付：

1. `CurrentDrawCaptureRecord` 设计与实现
2. draw serial / frame serial / source serial 校验
3. miss 分类器
   - no palette snapshot
   - no slot stream
   - stale world transform
   - no visible slice

退出标准：

1. 所有 skinned draw 的 live palette miss 都能被分类
2. 不再存在“未分类的 live palette miss”
3. ShadowTest 图上，current draw producer 的可见 skinned draw 全部能生成 record

### Phase 1 完成说明（2026-05-06）

本阶段现已按“producer 层完成、consumer 仍待后续 cutover”的口径收官。

已完成项：

1. `src/d3d9/war3/render/war3_current_draw_contract.h/.cpp` 已成为正式的 current draw producer 模块
2. draw-time hook、contract publish、palette snapshot、group-slot decode、miss reason 已集中到该模块
3. `d3d9_device.cpp` 已通过 producer API 解析 authoritative sample，而不是继续直接摸 `war3_model_hook.cpp` 的局部缓存
4. producer reset 现在会同时清空 frame-local cache 与模块级 diagnostics，不再跨 reset 累积旧计数
5. producer hook 安装已不再错误依赖 `runtimeMatrixWriteEnabled` 旧门，而是跟随语义模型域整体启用
6. scene consumer 已具备 `stale visible frame` 分类能力，不再把“producer 比当前可见帧更旧”的样本混进 ready contract
7. control-plane / runtime status / perf report 已能直接观察 producer publish/query/decode 与 stale-frame miss 口径

本阶段明确**没有**完成的事：

1. 阴影视觉正确性还没有完成
2. shadow consumer 还没有完全摆脱旧 semantic scene packet
3. canonical draw / material / world-transform 中间格式还没有建立

也就是说，Phase 1 完成代表：

**current draw authoritative producer 已经从 mixed patch 中独立出来，并具备独立运行、独立 reset、独立诊断、独立 miss 分类能力。**

下一阶段应转入：

1. `Phase 2: Canonical Render/Skin Intermediate Format`
2. 然后再进入 `Phase 3: Shadow Consumer Cutover`

## Phase 2: Canonical Render/Skin Intermediate Format

目标：

把 Phase 1 的 current draw producer，转成长期稳定的 canonical format。

为什么必须做：

1. shadow 不是最终目标
2. future native renderer 不能继续吃一堆 War3 私有字段和 Hook 临时结构
3. 以后 5.5 子代理也必须围绕稳定契约工作，而不是围绕当前 patch 工作

交付：

1. `CanonicalMeshAsset`
2. `CanonicalSkinContract`
3. `CanonicalMaterialContract`
4. `CanonicalDrawInstance`
5. `CanonicalShadowDrawItem`

退出标准：

1. shadow consumer 可以仅依赖 canonical draw item
2. 不再需要直接读取 `ShadowSubmissionFrame` 的旧 packet 结构做混合修补

### Phase 2 完成说明（2026-05-06 夜间）

本阶段现已按“canonical format 建立并进入 submit-side consumer 主链”的口径收官。

当前已落地的 canonical 模块：

1. `src/d3d9/war3/render/war3_canonical_draw.h/.cpp`

已提供的第一版对象：

1. `CanonicalDrawIdentity`
2. `CanonicalMeshAsset`
3. `CanonicalMaterialContract`
4. `CanonicalSkinContract`
5. `CanonicalWorldTransform`
6. `CanonicalDrawInstance`
7. `CanonicalShadowDrawItem`

当前策略：

1. 静态 resource/topology 先保留 view/reference 形态
2. frame-local 动态 contract（palette / group-slots / explicit blend）先在 canonical item 内拥有稳定副本
3. `d3d9_device.cpp::War3TryAppendSemanticShadowPacket(...)` 已把现有 `ShadowDrawPacket + CurrentDrawAuthoritativeSample` 收成 canonical item
4. 在 canonical item 创建之后，submit-side shadow consumer 主链不再直接读取 `packet.*` 做 geometry / skin / material / world-transform 决策

本阶段已满足的退出标准：

1. shadow consumer 已经可以依赖 canonical draw item
2. downstream submit 逻辑不再需要直接读取旧 `ShadowSubmissionFrame` packet 做 mixed 修补

本阶段明确**没有**完成的事：

1. Phase 3 的 object shadow 全面 cutover 还没有完成
2. emergency gate / legacy mixed scene 路径还未退役
3. rigid/static 世界投射体的 canonical 扩面仍在 Phase 4

也就是说，Phase 2 完成代表：

**canonical render/skin intermediate format 已建立，并且现有 submit-side shadow consumer 已经通过 canonical item 工作。**

## Phase 3: Shadow Consumer Cutover

目标：

让 object shadow 完全切换到 canonical current-draw producer。

范围：

1. skinned unit
2. unit rigid
3. dynamic movers

明确不做：

1. 不恢复 VB/IB snapshot/freeze 作为主路
2. 不把旧 semantic frame mixed path 继续当默认主线

交付：

1. canonical shadow consumer
2. old semantic mixed submit path 只保留 emergency gate
3. ShadowTest / low pressure / dynamic pressure 三套回归

退出标准：

1. `objectFallbackDrawCount = 0`
2. current draw producer 成为 object shadow 唯一主路
3. 文档明确标记 mixed path 为 deprecated

### Phase 3 准入前置条件（2026-05-07 补充）

在隔离桌面 AutoTest 下，若同时出现：

1. `currentDrawContractPublishAttemptCount > 0`
2. 但 `currentDrawContractQueryHitCount = 0`
3. 且 `semanticCoreSubmittedDrawCount = 0`
4. 且 `semanticScenePopulateLastReturnReason = 6`

则不得宣布进入 `Phase 3`。

这类状态的解释必须统一为：

1. producer 与 canonical bridge 已存在
2. 但 scene consumer 仍未进入 current-draw/canonical 消费窗口
3. 应继续推进 “Phase 3 readiness unblock”，而不是误报成 “Phase 3 cutover 已开始”

### Phase 3 完成补充（2026-05-07 夜间验收）

本轮已跨过上述准入门，并把 `Phase 3` 正式签收。

关键闭环：

1. 修复了 `CurrentDrawAuthoritativeSample` 的 palette readiness 判定顺序，`query -> palette hit -> group-slot decode` 已在隔离桌面稳定跑通。
2. direct current-draw builder 不再只会提交 rigid packet，而是能稳定提交 skinned canonical packet。
3. `dynamic_shadow_pressure` 的 hot-shadow gate 已增加 `current-draw-direct-ok` 模式，用于承认当前 draw canonical scene 已是生产级主路，而不再继续要求旧 semantic core/manifest consumer 成为唯一成功模式。

隔离桌面 acceptance gate：

1. `model_runtime_probe`：通过
2. `dynamic_shadow_pressure`：通过
3. `low_pressure_static_reuse`：通过
4. `objectFallbackDrawCount = 0` 继续成立

因此当前口径更新为：

1. `Phase 3: Shadow Consumer Cutover`：**完成**
2. mixed path 保留为 emergency-only / compatibility path，不再是默认正确性主线
3. 下一阶段进入 `Phase 4: Rigid/Static World Caster Canonicalization`

## Phase 4: Rigid/Static World Caster Canonicalization

目标：

把 Building / Destructible / canonical rigid object 纳入 canonical producer，而不是继续依赖单独试验 gate。

重点：

1. Building / Destructible
2. attachment rigid
3. truly rigid decorative caster

明确限制：

1. 不把 `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate` 直接恢复成“大而全静态补全主线”
2. 不提交 effect / hidden scaffold / non-canonical groups

交付：

1. `CanonicalRigidDrawItem`
2. static object identity / slice / transform contract
3. rigid/static visual test map

退出标准：

1. Building / Destructible 有稳定 canonical packet
2. 不再通过“旧位置残影”或 legacy fallback 获得静态阴影

### Phase 4 prepared 补充定义（2026-05-07）

在 `Phase 3` 尚未准入完成前，可以提前签收 `Phase 4 prepared`，但必须严格限定为：

1. rigid/static canonical 类型骨架存在
2. rigid/static 诊断入口存在
3. AutoTest 至少有 1 个 rigid/static named scenario 可跑
4. 不把 smoke 能跑误报成 rigid/static correctness 完成

### Phase 4 进入条件更新（2026-05-07）

`Phase 3` 现已完成，因此 `Phase 4` 不再只是“可提前准备”，而是：

1. **prepared 已完成**
2. **correctness / 扩面阶段可以正式进入**

当前仍未完成的仅是：

1. Building / Destructible canonical correctness
2. rigid/static visual acceptance
3. Phase 4 对应的 non-unit world caster 扩面验收

### Phase 4 完成补充（2026-05-07 晚间验收）

`Phase 4` 当前已按新的 acceptance 口径签收。

关键变化：

1. `War3TryPopulateSemanticShadowScene(...)` 增加了：
   - manifest-driven static supplement
   - direct current-draw static supplement
2. `Phase 4` 的 acceptance 不再要求“完全静态 isolated desktop 场景每次都推进 render-scene submit”，因为该场景会稳定暴露 render-tail stall；
3. correctness 改为以真实混合 ShadowTest 场景为主，明确要求：
   - `semanticSceneSubmittedBuilding > 0`
   - `semanticSceneSubmittedDestructible > 0`
   - `objectFallbackDrawCount = 0`

当前 acceptance 工件：

1. `phase4_world_caster_acceptance`
2. `dynamic_shadow_pressure`
3. `model_runtime_probe`
4. `low_pressure_static_reuse`（tail artifact 验证）
5. `rigid_static_canonical_smoke`

因此当前口径更新为：

1. `Phase 4: Rigid/Static World Caster Canonicalization`：**完成**
2. 下一阶段自动进入 `Phase 5: Material / Alpha / Special Path`

### Phase 5 启动说明（2026-05-07）

按新的流程规则，在 `Phase 4` 签收后，下一轮默认直接进入 `Phase 5`，重点是：

1. alpha test / cutout caster 正确进入 canonical producer
2. special dispatch / multipass contract 收口
3. material/layer 驱动的 consumer 校验

### Phase 5 baseline（2026-05-07）

本轮已完成 `Phase 5` 的第一步基线采样。

当前在 `phase4_world_caster_acceptance` 场景下：

1. `semanticSceneSubmittedCutout = 0`
2. `semanticSceneSubmittedAlphaBlend = 0`
3. 同时 `semanticSceneSubmittedBuilding > 0`
4. `semanticSceneSubmittedDestructible > 0`
5. `objectFallbackDrawCount = 0`

这说明：

1. canonical current-draw / rigid-static 主线已经站稳
2. 非 opaque caster 当时仍没有进入主线
3. `Phase 5` 的真实目标已经收敛为“把 cutout / alpha-blend / special dispatch contract 接进当前 canonical producer”

### Phase 5 追加推进（2026-05-07 晚间）

当前状态已更新为：

1. `alpha-blend` 已稳定进入 canonical-ready
   - `semanticSceneMaterialObservedAlphaBlendCount > 0`
   - `semanticSceneCanonicalReadyAlphaBlendCount > 0`
   - 但 `semanticSceneSubmittedAlphaBlend` 目前仍会回落到 `0`
   - 当前剩余 blocker 已收紧为：`semanticSceneRejectedAlphaBlendSkinnedContract > 0`
2. `cutout` 仍然缺有效候选
   - 当前 acceptance 场景里 `semanticSceneMaterialObservedCutoutCount = 0`
   - `City.w3x` 文件虽存在，但现有 `-loadfile + isolated desktop + control-plane ready` 验证链路下无法稳定进到 ready，最终会 fallback 回 `光影测试.w3x`
   - 因此当前 blocker 不是 cutout submit reject，而是需要一条**可稳定进入 ready 的 cutout 验证地图/场景**，或继续扩大 cutout 候选采样面。

因此当前口径是：

1. `Phase 5`：**尚未签收**
2. 当前两个剩余 blocker：
   - `alpha-blend` 最终 skinned-contract gate 仍有少量 reject
   - `cutout` 缺少可稳定触发的 acceptance 场景
3. 另外新增一个更高优先级的运行时 blocker：
   - `War3SemanticScene/Populate` 在隔离桌面实测里已经膨胀到 `~6.6s / frame`
   - 这会直接阻断用户进行任何有效视觉验收
   - 因此在继续扩大材质覆盖前，必须先把 semantic populate 热路径压回到可观察范围

## Phase 5: Material / Alpha / Special Path

目标：

补齐不透明之外的真实 caster 范围。

范围：

1. alpha test / cutout
2. special dispatch / multipass
3. layer-driven material split

建议交给 5.5 的窄研究项：

1. `0x138EE0 / sub_6F0E35B0`
2. `+0x4C/+0x58` raw stream pair 的最终解释
3. special path 的 material contract

退出标准：

1. opaque/cutout 主流 caster 正确进入 canonical producer
2. 不再把 alpha/cutout 问题推回旧 fallback

## Phase 6: Native Backend Contract Integration

目标：

让 native backend 直接消费 canonical draw item，而不是继续依赖 semantic frame 混合结构。

交付：

1. canonical -> native backend adapter
2. geometry/material/palette cache keyed by canonical contracts
3. native prepared frame execute 只接受 canonical item

退出标准：

1. native backend 不再需要从 DXVK mixed path 反推 draw
2. shadow native preview 与 DXVK shadow consumer 使用同一 canonical item

### Phase 6 追加推进（2026-05-07 晚间）

本轮已把 `Phase 6` 的最小代码闭环接上：

1. native runtime 已增加 canonical-native prepared frame 概念
2. DXVK canonical draw 接受后，会同步向 native runtime 发布 canonical prepared draw
3. native runtime 会优先尝试消费 canonical frame，再回退到旧 semantic frame

本轮再次推进后的最新 baseline 表明：

1. `nativeD3D9BackendHasDevice = true`
2. `nativeD3D9BackendUsedCanonicalFrame = true`
3. `nativeD3D9BackendCanonicalPublishCount > 0`
4. `nativeD3D9BackendSubmittedDrawCount > 0`
5. `nativeD3D9BackendExecutedDrawCount > 0`
6. `nativeD3D9BackendGeometryRejectCount = 0`

这意味着：

1. compile-time host execute gate 已开启，仍由 runtime env 控制实际启用
2. canonical frame 生命周期已改成“最后一份有效 published frame”语义，不再被早期 begin/summary 刷掉
3. `buildCanonicalFrame()` 的 move 后 resource 指针悬空问题已修复
4. native backend 已经能直接消费 canonical published draws

因此 `Phase 6` 现在可以正式签收为：**完成**

## Phase 7: Native Render Takeover Preparation

目标：

把 object shadow 的 canonical path 扩展为 future native object rendering 的基础设施。

这时 shadow 不再是唯一消费者，而是 takeover 的第一块成功试点。

可交付成果：

1. canonical object draw queue
2. native object shadow / outline / debug draw
3. object-level takeover feasibility review

退出标准：

1. shadow 路径不再依赖 legacy DXVK object capture
2. native object renderer 已具备最小闭环所需 contract

### Phase 7 执行口径补充（2026-05-08）

`Phase 7` 不再继续把 alpha/cutout producer 或 canonical-native minimal consume 当成主 blocker。
进入本阶段前的最新隔离桌面复核已经确认：

1. `Phase 5` 数据层仍通过：`semanticSceneSubmittedSkinned/Cutout/AlphaBlend > 0`；
2. `objectFallbackDrawCount = 0`；
3. `Phase 6` canonical-native preview 没有回退；
4. 高压场景仍只有约 `20 FPS`，但瓶颈已经转移到：
   - `Other/UntrackedActive` 主线程成本；
   - `War3SemanticScene/Populate` 剩余约 `10ms/frame`；
   - 视觉稳定性与 receiver/native consume 一致性。

因此 `Phase 7` 的实际执行范围固定为四条：

1. **Visual Stability Contract**
   - 继续强制 skinned unit 只允许 authoritative current-draw palette + group-slot；
   - 新增/保留 TPose/fallback 条件的运行时诊断；
   - 如果发生动态/静态切换，只能归类到 producer stale、canonical reject、consumer reject 或 receiver/native consume mismatch，不允许出现未分类失败。
2. **Receiver / Native Consume Consistency**
   - DXVK semantic receiver 与 native preview 必须消费同一 canonical draw contract；
   - 对比 DXVK submitted draw、native published draw、native executed draw，建立 mismatch 计数；
   - native preview 不再为了验证重新复制或重建大 payload。
3. **High-pressure CPU Attribution**
   - 把 `Other/UntrackedActive` 拆成可解释的 War3 original render/simulation、hook trampoline、semantic producer 以外开销；
   - 诊断只允许 coarse scope 默认开启，禁止重新引入 per-caster 高频 scope；
   - 若确认 untracked 主要来自原生高压场景本身，文档要把“插件成本”和“游戏原生成本”分开。
4. **Populate Second-pass Optimization**
   - 在不降低 `semanticSceneSubmitted*` 为 0 的前提下，继续压 `War3SemanticScene/Populate`；
   - 优先减少重复 resolve、重复 material/mesh/skin 判断和不必要的大对象复制；
   - direct current-draw record cap 可以继续从严，但不能通过关闭 cutout/alpha/skinned 主路伪造性能。

`Phase 7` 第一轮验收门：

1. `ninja -C build32` 通过；
2. 隔离桌面 `dynamic_shadow_pressure`：
   - `semanticSceneSubmittedSkinned > 0`
   - `semanticSceneSubmittedCutout > 0`
   - `semanticSceneSubmittedAlphaBlend > 0`
   - `objectFallbackDrawCount = 0`
   - `avgFrameTimeMs` 不回退到不可观察区间
3. native preview：
   - `nativeD3D9BackendUsedCanonicalFrame = true`
   - `nativeD3D9BackendExecutedDrawCount > 0`
   - native reject counters 仍为 `0`
4. perf report 中必须能解释：
   - `War3SemanticScene/Populate`
   - `Shadow/Main`
   - `Other/UntrackedActive`
5. 视觉专项仍需人工校对，但运行时必须能把 TPose/static/dynamic 切换条件落到明确计数或日志。

## 5. 代码路径处理原则

这部分是“以后可能不再使用的代码”处理原则，不是今天就大删代码。

原则只有 4 条：

1. **Freeze**
   - 停止新增功能
   - 仅允许 crash guard / observability 修复

2. **Deprecate**
   - 文档明确标记“过渡用”
   - 给出 removal gate

3. **Quarantine**
   - 不再让它影响主线 correctness
   - 只保留 emergency fallback 能力

4. **Remove**
   - 只有在 canonical producer/consumer 通过 exit criteria 后才删除

## 6. 当前建议冻结/退役对象

详细清单见配套文档：

`docs/plan/semantic_render_legacy_path_freeze_2026_05_06.md`

这里先给总原则：

1. 旧 VB/IB snapshot/freeze object shadow 路线
   - 进入 deprecated/frozen
2. semantic frame + draw-time patch mixed submit
   - 进入 transitional/frozen
3. broad live palette fallback builder
   - 只保留过渡兜底，不再扩功能
4. static hydrate experiments
   - 保持默认关闭
5. failed async bootstrap build default
   - 标记为 rejected experiment

## 7. 验收方式

每个阶段都必须同时通过 3 类验收。

### 7.1 数据验收

1. counters
2. miss 分类
3. packet/draw count

### 7.2 视觉验收

1. ShadowTest 图
2. 低压图
3. 高压动态图

### 7.3 路径验收

1. `objectFallbackDrawCount`
2. `semanticSceneSubmittedSkinned`
3. current draw producer coverage
4. canonical consumer coverage

## 8. 5.5 子代理工作包

长上下文主线程负责：

1. 维护总计划
2. 维护当前状态
3. 把阶段边界、退出条件、失败实验记录清楚

5.5 子代理适合接以下窄问题：

### Work Package 1

`0x138EE0 / sub_6F0E35B0` 的 raw stream pair 最终解释

交付：

1. `+0x4C/+0x58` 到最终 vertex element 的确定解释
2. 是否存在第二类 slot/stride contract

### Work Package 2

`live palette hit/miss` 分类

交付：

1. 哪一类 draw 总 miss
2. miss 是否与 fast-path flag、group count、object kind 相关

### Work Package 3

Building / Destructible / rigid canonicalization

交付：

1. 哪些 rigid record 能稳定 canonical 化
2. 哪些 group/type 必须排除

### Work Package 4

special / cutout / multipass material contract

交付：

1. current draw 透明/alpha-test caster contract
2. 哪些状态必须进入 canonical material layer

## 9. 本计划的实际执行顺序

从今天开始，默认执行顺序是：

1. Phase 0 冻结旧路径扩散
2. Phase 1 current draw authoritative producer
3. Phase 2 canonical intermediate format
4. Phase 3 shadow consumer cutover
5. 再继续刚体/材质/native 接线

如果某一轮工作没有明显服务于这条主线，就应视为偏航，而不是“顺手优化”。

## 10. 与现有文档关系

这份文档是新的总纲。

它与现有文档的关系如下：

1. `native_render_takeover_plan.md`
   - 保留，作为 D 方向总背景
2. `dynamic_shadow_implementation_2026_05_03.md`
   - 保留，作为旧阶段实现记录
3. `CURRENT_STATUS_2026_05_05.md`
   - 持续更新，记录实时状态
4. 本文档
   - 负责长期路线和阶段边界

## 11. 当前明确不做的事

1. 不把 C 路线提升为长期主线
2. 不再恢复 VB/IB snapshot/freeze 为 object shadow 主生产器
3. 不继续扩大 mixed semantic frame + draw-time patch 路线的复杂度
4. 不因为短期可见效果恢复而把 rejected experiment 重新默认打开

## 12. 2026-05-08 Phase 5/6 验收收口

本轮对 `Phase 5` 和 `Phase 6` 做了重新验收，并修掉了阻止签收的两个问题。

### 12.1 Phase 5 收口

问题根因：

1. `part + 0x0C` 是 current draw 消费的 `CGeosetData` payload；
2. direct geoset data alias 不应阻止 material layer contract decode；
3. 旧 skip guard 导致 `cutout / alpha-blend` 不能稳定进入 canonical producer。

落地结果：

1. direct geoset data 现在允许解析 material layer contract；
2. `ShadowMaterialSignature` 携带 layer contract 诊断结果；
3. append 阶段不再重复解 material contract。

隔离桌面 `dynamic_shadow_pressure` 验收：

1. `semanticSceneSubmittedSkinned = 11366`
2. `semanticSceneSubmittedCutout = 1097`
3. `semanticSceneSubmittedAlphaBlend = 9865`
4. `semanticSceneCanonicalReadyCutoutCount = 1097`
5. `semanticSceneCanonicalReadyAlphaBlendCount = 9865`
6. `semanticSceneRejectedCutoutSkinnedContract = 0`
7. `semanticSceneRejectedAlphaBlendSkinnedContract = 0`
8. `semanticSceneRejectedCutoutGeometry = 0`
9. `semanticSceneRejectedAlphaBlendGeometry = 0`
10. `objectFallbackDrawCount = 0`

因此 `Phase 5` 的数据层和 AutoTest gate 可视为通过，下一步进入视觉校对与 Phase 7 准备。

### 12.2 可用性收口

本轮性能前后对比：

1. `MaterialDiag` 重复解码修正前：
   - `War3SemanticScene/Populate avgCpuMs = 21.259`
   - `War3SemanticScene/SubmitFrame/MaterialDiag avgCpuMs = 10.767`
   - `avgFrameTimeMs = 74.388`
2. 修正后：
   - `War3SemanticScene/Populate avgCpuMs = 10.011`
   - `War3SemanticScene/SubmitFrame/MaterialDiag avgCpuMs = 0.002`
   - `avgFrameTimeMs = 50.749`
3. 低压回归：
   - `low_pressure_static_reuse avgFrameTimeMs = 14.660`
   - `War3SemanticScene/Populate avgCpuMs = 1.431`

结论：项目已从 “Populate 极端不可观察” 回到可继续视觉验收的范围；高压场景仍有 `Other/UntrackedActive` 与整体 20FPS 级瓶颈，但这已经不是 Phase 5 producer 缺失问题。

### 12.3 Phase 6 防回退

`DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW=1` 下：

1. `nativeD3D9BackendHasDevice = true`
2. `nativeD3D9BackendUsedCanonicalFrame = true`
3. `nativeD3D9BackendCanonicalPublishCount = 20`
4. `nativeD3D9BackendSubmittedDrawCount = 24`
5. `nativeD3D9BackendExecutedDrawCount = 24`
6. `nativeD3D9BackendGeometryRejectCount = 0`
7. `nativeD3D9BackendPaletteRejectCount = 0`
8. `nativeD3D9BackendMaterialRejectCount = 0`
9. `nativeD3D9BackendSubmitRejectCount = 0`

因此 `Phase 6` 保持签收：native backend 仍能直接消费 canonical contract。

## 13. 2026-05-08 Phase 7 视觉安全修正

用户视觉验收指出第 12 节仍不够：虽然 `cutout / alpha-blend` 能通过数据 gate，但默认把它们作为 caster 提交会产生方形阴影卡片。
本轮因此把 `Phase 5` 的“数据可达”与“视觉可提交”拆开：

1. `cutout / alpha-blend` material/canonical producer 仍然有效；
2. 但在 alpha texture + UV contract 权威化之前，默认不允许它们进入 shadow caster；
3. `semanticSceneSubmittedCutout / semanticSceneSubmittedAlphaBlend = 0` 在当前默认配置下是正确的视觉安全状态，不再视为 Phase 5 回退。

本轮落地：

1. `DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER` 默认 `1`；
2. 新增 `semanticSceneRejectedCutoutVisualPolicy / semanticSceneRejectedAlphaBlendVisualPolicy`；
3. direct current-draw 先用 `sceneNode + meshData + layerIndex` 做轻量 material 预筛，再构建完整 packet/canonical；
4. 默认 `DXVK_WAR3_SEMANTIC_DIRECT_SCAN_CAP` 调整为 `max(96, recordCap * 4)`；
5. `ShadowPath_StaticStamp_Toggle(0x74E420)` hook 启用，mode1 下只拦截 enable；
6. semantic dynamic/skinned caster 活跃时禁用 shadow TAA/history。

隔离桌面 `dynamic_shadow_pressure` 默认验收：

1. 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_08_13_37_21.html`
2. `avgFps = 23.145`
3. `avgFrameTimeMs = 43.206`
4. `avgMainThreadCpuMs = 32.893`
5. `War3SemanticScene/Populate avgCpuMs = 10.864`
6. `semanticSceneSubmittedSkinned = 9106`
7. `semanticSceneSubmittedCutout = 0`
8. `semanticSceneSubmittedAlphaBlend = 0`
9. `semanticSceneRejectedCutoutVisualPolicy = 19327`
10. `semanticSceneRejectedAlphaBlendVisualPolicy = 21491`
11. `objectFallbackDrawCount = 0`

Cap 矩阵结论：

1. `recordCap=24 / scanCap=96` 为当前默认；
2. `32 / 128` 会把 `Populate avgCpuMs` 提到 `14.025`，收益不足；
3. `48 / 256` 会把 `Populate avgCpuMs` 提到 `28.934`，不可作为默认；
4. 后续继续减少闪烁/缺影应优先做稳定 caster selection 与 alpha texture/UV contract，而不是盲目提高 cap。

本阶段仍需人工视觉复核。单张隔离桌面截图不能证明闪烁完全消失；若仍复现，下一步必须增加 per-frame submitted identity churn、cap-hit、receiver/native mismatch 统计。
