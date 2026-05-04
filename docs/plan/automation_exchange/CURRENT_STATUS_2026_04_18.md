# Current Status

Date: 2026-04-18

## 1. Executive Summary

项目已经推进到：

1. 单位阴影已经不再是“完全没有”
2. `ShadowRendererCore -> semantic scene submission` 已经真实接管了大量单位对象
3. 旧 fallback 已被明显削减
4. 后台最新几轮截图里，已经不再复现“只在第一帧看到阴影，随后整局消失”

但当前仍未完成，因为最关键的问题变成了：

1. 单位阴影形状错误
2. 当前单位阴影呈现为撕裂、方块状或不正确的硬块阴影
3. 主线程 CPU 仍然极高，项目还不可用

## 2. What Is Already Done

### 2.1 Control plane / AutoTest

已经完成：

1. DLL named pipe control plane
2. `ping / get_runtime_status / wait_until / get_shadow_runtime_summary / get_frame_manifest_summary / capture_final_frame / invoke_test_command / shutdown_session`
3. 后台隔离桌面 AutoTest 验证链

### 2.2 Semantic data chain

已经完成：

1. `VisibleRenderableManifest`
2. `ShadowModelResourceStore`
3. `ShadowPoseStore`
4. `ShadowRendererCore`
5. `DxvkValidationBackend`

### 2.3 Units-first semantic scene submission

已经完成：

1. `ShadowRendererCore` 每帧构建 semantic packet
2. `BeforeUi` 前把单位 packet 落到 `shadowInstances / shadowCasters`
3. 单位旧 fallback 已开始被 semantic scene 顶掉

### 2.4 This round's concrete progress

本轮已经实际落地并后台验证的点：

1. `dynamic-mesh rescue` 改成优先使用 `sceneNode + 0x64` 世界矩阵
2. `dynamic-mesh` 的几何上传改成 frame-local，而不是 persistent geometry 爆池
3. semantic packet 与 legacy fallback 的 `batchHandle` 已统一格式
4. `UpperLayerShadowConsumer` 观测链已关闭，避免继续并行消耗
5. `ShadowValidationRuntime` 和 `ShadowRuntimeContractCache` 的 frame/resource 复制已进一步改成 shared snapshot，减少重复拷贝

### 2.5 New progress in this handoff

本轮新增已落地并后台验证的点：

1. 修正了 `ShadowRendererCore::TryResolveMeshDynamicIndexStream()` 对
   `meshData + 0xE0` 的错误解释：
   - 不再把它当成“dynamic index buffer 指针”
   - 改为按 `primitive base index` 语义，从静态 geoset index 中截取当前 draw slice
2. `dynamic-mesh rescue` 现在会优先使用：
   - `meshData + 0xE0 = primitive base index`
   - `meshData + 0xC8/+0xCC = sub-primitive count/pairs`
   来决定当前 packet 的 index 切片，而不再默认整条 geoset index 全量重放
3. 语义验证栈新增一层止血：
   - 在 `kShadowSemanticCoreSceneSubmissionEnabled=true` 时
   - `runObserveValidation()` 不再额外跑一遍 hash-only `DxvkValidationBackend::submitFrame()`
   - 避免与后续 `BeforeUi -> semantic scene submission` 做重复 packet 遍历
4. 所有验证继续走后台隔离桌面 AutoTest，没有占用前台窗口。

### 2.6 Latest round progress in this handoff

本轮再次新增已落地并后台验证的点：

1. `ShadowRendererCore` 新增了对 `meshData + 0x4C/+0x58` 的
   `dynamic group-slot` 提取：
   - 不再只把它当诊断日志观察
   - 当 `stream1` 首字节序列能和当前 pose palette 对齐时，
     会优先覆写 packet 的 `vertexGroupIndices`
2. `dynamic-mesh rescue` 不再一律退回 rigid packet：
   - 当 `stream1` 能直接映射到当前 pose palette 时
   - 会直接升格为 `dynamic skinned rescue`
3. 删掉了 `meshData + 0x0C` 作为 `dynamicVertexCount` 的复用：
   - 该字段当前已经被拿来当 primary stream 的 stride 候选
   - 再把它同时当 vertex count 语义不一致，容易继续放大块状/撕裂阴影
4. 本轮代码改动集中在：
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
5. 本轮验证继续全部走隔离桌面后台启动，并且结束时使用
   `avoid_foreground_switch=True` 静默关闭，没有抢前台。

### 2.7 Newly verified in this handoff

本轮又新增了两类“已经写进代码并被后台验证到明确结论”的点：

1. 已把 `primitiveRecords / topology / dynamic index hash` 正式穿进：
   - `ShadowRuntimeContract`
   - `ShadowRendererCore`
   - `War3TryAppendSemanticShadowPacket()`
   - 不再让 semantic geometry key 完全忽略 dynamic index slice
2. 已尝试把 `meshData + 0xF0` 接入为 `meshPoseCtx` 候选：
   - `TryResolveMeshPoseContext()` 已接到 dynamic rescue 与 runtime-group palette rescue 分支
   - 但后台 DBWIN 日志已明确证伪这条当前假设
3. 后台验证确认：
   - `meshData + 0xF0` 在当前这些单位样本里表现为 `0x01 / 0x04 / 0x15 / 0x2E / 0x4F` 这类小整数
   - 当前并不是 `runtimeModel*`
   - 所以本轮新增的 `meshPoseCtx -> PoseRegistry` 路线没有真正命中，`meshPose=0`
4. 后台验证同时确认：
   - `dynamic-mesh rescue` 日志里 `baseIdx` 仍经常出现明显离谱的大值
   - 代表 `meshData + 0xE0` 不能在这些 unit path 上无条件当作通用的 `primitive base index`
   - 当前 dynamic rescue 在这些样本上仍然经常落成 `dynIdx=0`
5. 换句话说：
   - `topology / dynamic index hash` 的宿主接线已经补上
   - 但 authoritative `index/topology source` 仍未被真正拿到
6. 本轮后台验证证据：
   - `ninja -C build32` 通过
   - 隔离桌面 AutoTest control plane 摘要：
     - `semanticCoreResolved = 363`
     - `semanticCoreSkinnedResolved = 363`
     - `semanticCoreSkippedNoRuntimeGroupPalette = 38`
     - `visibleCount = 918`
     - `unitCount = 287`
   - DBWIN probe：
     - `dynamic-mesh rescue = 10`
     - `runtime-group miss = 23`
     - `dynamic-group override = 0`

### 2.8 Latest round progress in this handoff

本轮继续在 `ShadowRendererCore` 上补了两类真实实现，并完成了两轮隔离桌面后台验证：

1. `TryResolveMeshDynamicIndexStream()` 不再只依赖：
   - `meshData + 0xE0` 直接切 `resource.indices`
   - 或仅把 `subPrimitivePairs[i].second` 累加成 `indexCount`
2. 当前已经改成：
   - 先把 `meshData + 0xC8/+0xCC` 当成 `sub-primitive record` 候选读出
   - 再优先用它去和 `resource.primitiveRecords` 做“同起点匹配 / 全表唯一匹配”
   - 当 `baseIdx` 明显离谱时，允许回退到“按 primitive 序列反定位真实 draw slice”
3. 这意味着：
   - `meshData + 0xE0` 不再是唯一 authoritative 起点
   - 当前 unit path 即使 `baseIdx` 异常，也有机会通过 `sub-primitive` 序列找回真实 `firstIndex/indexCount/topology`
4. 同时修掉了一个已经存在但尚未真正生效的 bug：
   - `resolveRecord()` 里 authoritative skinned 成功后的 `dynamic-group override`
   - 原来错误地检查 `outPacket.runtimeGroupPalette.empty()`
   - 但真正的 palette 赋值发生在更后面，导致这条 override 分支实际近似死路
   - 本轮已改成直接使用局部 `runtimeGroupPalette` 判定与上限检查
5. 另外把 `runObserveValidation()` 里 `UpperLayer` 的空转合流做成了编译期门控：
   - 当前 `kUpperLayerShadowConsumerEnabled=false`
   - 因此不再为一个空的 `UpperLayerShadowRegistry` 建 `dedupKeys + merge` 流程
6. 本轮两次后台验证证据：
   - `ninja -C build32` 在两次改动后都通过
   - `dynamic_shadow_pressure`：
     - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_03_10_06.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260418_031007.png`
     - `avgMainThreadCpuMs = 7488.281`
     - `fallbackDrawCount = 946`
   - `low_pressure_static_reuse`：
     - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_03_12_33.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260418_031234.png`
   - `avgMainThreadCpuMs = 670.536`
   - `fallbackDrawCount = 3253`

### 2.9 Latest round progress in this handoff

本轮继续往 `stream1/aux binding` 这个主 blocker 上推进了一步，并把结果缩到“代码已落地 + summary 已验证，但最终帧抓图链当前仍有 blocker”这个真实状态：

1. `ShadowRenderableRecord` 现在正式携带 `layerIndex`：
   - 已从 `VisibleRenderableRecord` 复制到 semantic contract
   - 后续如果要把 `meshData + 0x94 aux_layer_resource_table` 和 layer dispatch 真正接进 core，不再需要回头补 contract 缺口
2. `ShadowRendererCore` 新增了 `stream1 packed slot tuple` 解释路径：
   - 不再只把 `stream1[i * stride]` 的首字节当作 group slot
   - 会先尝试把每个顶点的 `stream1` 前 4 字节解释成 `packed runtime slot tuple`
   - 对每个 unique tuple 直接生成一份动态 `runtimeGroupPalette` 条目，并按 tuple 内 slot 做等权平均
3. 这条新路径已经同时接到：
   - authoritative skinned path 的 `dynamic-group override`
   - `tryDynamicMeshRescue()` 的 dynamic skinned rescue
4. 当前代码上的目标很明确：
   - 避免继续把 `0x07030502 / 0x24141313` 这类样本压成单字节 slot
   - 尽量把 `stream1` 从“首字节猜测”推进到“packed tuple -> dynamic runtime group palette”
5. 本轮实际改动文件：
   - `src/d3d9/war3/shadow/war3_shadow_runtime_contract.h`
   - `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
6. 本轮验证结果分两部分：
   - `ninja -C build32` 通过
   - summary-only 后台 control plane 验证通过：
     - `semanticCoreResolved = 366`
     - `semanticCoreSkinnedResolved = 366`
     - `semanticCoreSkippedNoRuntimeGroupPalette = 39`
     - `visibleCount = 921`
     - `unitCount = 288`
     - `recordsWithRuntimeModel = 442`
7. 但这轮还不能宣称“单位阴影形状已转正”，因为：
   - 隔离桌面下 `capture_final_frame` 仍然超时
   - 本轮也没有生成新的 `war3_perf_report_auto_*.html`
   - 当前最新自动报告仍停在 `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_03_12_58.html`
8. 换句话说：
   - 本轮代码已经把 `stream1 packed tuple` 路线真正接进 semantic core
   - 运行时 summary 证明 semantic core 仍在真帧上工作
   - 但最终帧视觉正确性本轮还没有被新的后台截图重新确认

### 2.10 Latest round progress in this handoff

本轮继续往“单位阴影不再消失 + skinned scene submission 提高占比 + 找到 scene 提交真实失败原因”这三件事推进，并且都已经通过后台隔离桌面验证：

1. 修掉了一个已经被后台 control plane 明确复现的问题：
   - `VisibleRenderableManifest` 在热帧里会丢失后续 identity
   - 表现为 `unitCount` 先正常，随后掉成 `0`，大量对象退回 `Unknown`
   - 现在已经在：
     - `war3_visible_renderables.cpp`
     - `war3_shadow_runtime_contract.cpp`
   - 增加了 prior-frame identity carryover / repair
2. 后台 control plane 新样本已经确认：
   - `unitCount` 不再在热帧里掉成 `0`
   - 晚于首帧的后台截图里，单位阴影仍然存在
   - 也就是说，“只在第一帧看到阴影，随后全程消失”当前在后台验证链里已经不再是主症状
3. `ShadowRendererCore::tryDynamicMeshRescue()` 已新增更保守的
   `static skinned rescue`：
   - 当静态 geoset 顶点数和当前动态路径一致
   - 且当前 draw 不依赖 dynamic index stream
   - 会优先回到 `static geoset + runtime palette`
   - 不再无条件走 frame-local dynamic positions upload
4. `War3ShouldSubmitSemanticPacket()` 也放宽到了：
   - 只要 semantic core 已经把 packet 解成 `Skinned`
   - 就允许在 `unitsOnly` 模式下进入 scene submission
   - 不再强依赖 `objectKind == Unit` 或 runtimeModel 句柄存在
5. 本轮新增的 scene submission failure counters 已经落地：
   - `semanticSceneRejectedNoVertex`
   - `semanticSceneRejectedSkinnedContract`
   - `semanticSceneRejectedGeometry`
6. 最新后台报告：
   - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_19_46_31.html`
   - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_19_49_05.html`
   已经给出几个关键新结论：
7. 这轮的正向变化：
   - `semanticSceneSubmittedSkinned` 已从 `40` 抬到 `156~160`
   - `staticPersistentCount` 已从 `0` 抬到 `116~120`
   - `uniqueGeometryCount` 已经不再是 `0`，而是 `44~46`
   - 说明一部分 skinned/unit packet 已经开始真正走 persistent static geometry，而不是全部退回 frame-local 动态上传
8. 这轮的新诊断结论：
   - `semanticSceneRejectedGeometry = 0`
   - `semanticSceneRejectedNoVertex = 0`
   - `semanticSceneRejectedSkinnedContract = 48`
   - 这说明 scene submission 当前并不是主要死在“buffer 创建/上传失败”
   - 而是仍有一批 skinned packet 在最终 vertex-group / palette contract 上没过校验
9. 但性能结论仍然没有转正：
   - `avgFps` 仍在 `0.239 ~ 0.244`
   - `avgMainThreadCpuMs` 仍在 `3835 ~ 3906 ms`
   - 热点仍然集中在：
     - `Hook_FlushAndReset/Orig ~= 2658 ms`
     - `Hook_WorldObjects_RenderGroup/Orig ~= 1278 ms`
     - `SceneCollector/RegisterBatch ~= 48 ms`
10. 当前这轮最重要的阶段判断是：
    - “单位阴影首帧后消失”这条主断点已经被往前推进并且后台不再稳定复现
    - `semantic skinned submission` 占比也已经明显抬高
    - 但视觉正确性还没转正，性能也还没有回到可用区间

### 2.11 Latest round progress in this handoff

本轮继续直接针对“单位阴影仍像 rigid/静态硬块，达不到旧 VB/IB 捕获画质”这个问题推进，已经落地的点有：

1. `ShadowDrawPacket -> War3TryAppendSemanticShadowPacket -> shadow pass`
   这条链现在正式支持显式 `blend weights + blend indices`：
   - `ShadowPacketResource` 新增：
     - `explicitBlendCount`
     - `vertexBlendWeights`
     - `vertexBlendIndices`
   - semantic scene append 不再只能构造
     `blendCount=0 + 单 slot rigid group`
   - 现在已经能上传一条独立的 `War3SemanticShadowBlend` vertex stream，
     其中包含：
     - `vec3 weights`
     - `u8x4 indices`
   - 并把它真正喂给 `PaletteSkinnedFF`
2. `ShadowRendererCore` 里已经新增一条新的 skinned contract 尝试：
   - `stream1` 提供 ordered compact indices
   - `aux0 (8-byte sideband)` 提供显式 float-like weight seeds
   - 若命中，就不再走“group matrix averaging + 单 slot”旧近似
   - 而是直接产出显式 blend packet
3. 这条 explicit blend 路线已经不再只挂在“runtime group palette 已成功”之后：
   - 现在会在 `runtime-group miss` 前先尝试一次
   - 目的是直接救回一部分原本会掉进 `skippedNoRuntimeGroupPalette`
     的单位
4. 同时补了一条更保守的本地 remap fallback：
   - 若 `stream1` 的 compact slot 本身超出当前 pose palette
   - 但 unique tuple 数量又能落进当前 pose 数量
   - 当前会先按局部 `0..N-1` remap 试一次
   - 作为 `l1cHop remap/span` 彻底解完前的保守替代

### 2.11.1 Validation result of this round

本轮后台验证继续全部走隔离桌面，没有占用前台：

1. `ninja -C build32` 多次通过
2. 手动 control plane 背景验证：
   - `get_shadow_runtime_summary`
   - `get_frame_manifest_summary`
   - `get_runtime_events`
3. 代表性运行态样本：
   - `semanticCoreResolved = 370`
   - `semanticCoreSkinnedResolved = 370`
   - `semanticCoreSkippedNoRuntimeGroupPalette = 60`
   - `visibleRenderableCount = 926`
4. 但这轮还不能宣称“画质已经追上旧 VB/IB 路线”，因为：
   - `explicit-blend skin` 事件在当前后台样本里还没有命中
   - `dynamic-group override` 事件仍是 `0`
   - `runtime-group miss` 仍稳定出现
5. 当前最新的真实状态是：
   - 显式 blend contract 已经进入代码与后端消费链
   - 但当前后台样本还没有证明它已经命中 live unit path
   - 这意味着 blocker 进一步收敛成：
     - `stream1 + aux0` 的 live 命中时机
     - 以及 `l1cHop / remap-span` 仍未完全解开

### 2.11.2 Best next step after this round

下一轮最该继续做的是：

1. 不再只看 `runtime-group miss`，而是直接增加一层
   `explicit blend candidate miss` 诊断，确认当前没命中的具体原因：
   - 没有 `layerContract`
   - `aux0` 不可读
   - `stream1` stride/offset 搜索失败
   - tuple count 超界
   - local remap 失败
2. 把 `l1cHop` 真正接到 explicit blend 路线里：
   - 让高位 compact slot 不再只能靠“局部 remap fallback”
   - 而是能通过真实 remap/span 表转成 pose-local slot
3. 在 explicit blend 真正命中 live unit path 后，再继续收：
   - 单位阴影视觉正确性
   - `Hook_FlushAndReset/Orig`
   - `Hook_WorldObjects_RenderGroup/Orig`
   这两条当前主线程热链

## 3. Verified Reality

### 3.1 Functionality

后台截图已经能看到单位阴影存在，代表“单位完全没阴影”这个阶段已经过去了。

但当前阴影仍然是错误形态，不可视为完成。

### 3.2 Current defect

当前最准确的问题表述是：

1. 单位 shadow 已经出现
2. 但形状仍然是撕裂的、方块状的，或者明显不是正确模型阴影
3. 本轮修掉 `meshData + 0xE0` 的旧误读后，阴影没有退回“完全不可见”，
   但后台截图仍能看到明显偏块状/硬边的单位影子，说明 blocker 还没解除
4. 本轮把一部分单位 packet 从“dynamic rigid rescue”推进到了
   “dynamic skinned packet”，但最终截图里近景单位阴影仍然带明显方块状投影，
   说明 `stream1` 接入还不是完整解法
5. 本轮新增验证还说明：
   - `meshData + 0xF0` 不是当前这批 unit 的 `runtimeModel*`
   - 所以 `transform_or_pose_ctx` 这条线至少不能继续按“直接 pose 指针”理解
6. 同时：
   - `meshData + 0xE0` 在多条 `dynamic-mesh rescue` 日志里仍是大到离谱的值
   - 当前 unit path 经常没有真正拿到 `dynIdx`
   - 说明当前错误形态仍很可能与“真实 index/topology contract 缺失”直接相关
7. 本轮最新低压截图 `war3_20260418_031234.png` 进一步确认：
   - 单位阴影仍然明显带“大块方形底板”
   - 不是正常按模型轮廓收敛的 object shadow
   - 说明即使 `sub-primitive` slice 回收与 `dynamic-group override` 已接通，
     当前视觉正确性 blocker 仍未解除
8. 本轮新增验证还说明：
   - `semanticCoreResolved / semanticCoreSkinnedResolved` 继续维持在 `366`
   - `frame manifest` 仍然是 `visible=921 / unit=288 / runtimeModel=442`
   - 所以 semantic submission 主路径并没有掉
   - 但新的 packed-tuple 解释路径，本轮还没有通过最终帧截图完成视觉层闭环确认
9. 另外，本轮后台验证链本身还有一个独立 blocker：
   - `capture_final_frame` 现在已经能再次在隔离桌面 control plane 下成功返回
   - 但最新抓图尺寸仍然是 `1902x963`，没有回到 `2560x1440` 基线
   - 因此“功能是否转正”和“后台抓图/报告链是否完全稳定”现在仍要分开看

这意味着：

1. `semantic visibility`
2. `runtime pose`
3. `scene submission`

这些基础链路已经基本打通；
真正没打通的是“单位当前帧动态几何的完整 contract”。

### 3.3 Performance

当前性能仍然很差。

最近一轮纯后台 perf 报告：

1. 修复 `primitive base index` 之前：
   `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_01_52_27.html`
2. 本轮修复后：
   `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_01_56_06.html`

其中最关键的数据是：

1. 修复前：
   - `avgFps = 0.150`
   - `avgFrameTimeMs = 6676.954`
   - `avgMainThreadCpuMs = 6519.531`
2. 修复后：
   - `avgFps = 0.159`
   - `avgFrameTimeMs = 6302.170`
   - `avgMainThreadCpuMs = 6181.250`
3. 语义提交仍然大量存在：
   - `semanticSceneSubmitted = 1250`（5 帧窗口累计）
   - `semanticSceneSubmittedUnit = 1250`
   - `semanticSceneSubmittedSkinned = 105`
4. 旧 fallback 仍未收干净：
   - `fallbackDrawCount = 1184`
5. 当前热点仍然集中在：
   - `Hook_FlushAndReset/EndFrame`
   - `Hook_WorldObjects_RenderGroup/Orig`
6. `UpperLayer` 观测链仍然是 `0`，说明它不是当前主开销源

本轮新增后台隔离桌面验证：

1. 手工 control plane + screenshot + silent stop：
   `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_02_13_01.html`
2. 同轮 control plane 摘要：
   - `semanticCoreResolved = 87`
   - `semanticCoreSkinnedResolved = 87`
   - `semanticCoreRigidResolved = 0`
   - `semanticCoreSkippedNoRuntimeGroupPalette = 1`
3. 同轮 frame manifest 摘要：
   - `visibleCount = 369`
   - `unitCount = 85`
   - `recordsWithRuntimeModel = 125`
   - `recordsWithResolvedGeoset = 155`
4. 同轮 perf：
   - `avgFps = 1.827`
   - `avgFrameTimeMs = 547.391`
   - `avgMainThreadCpuMs = 530.488`
5. 相比上一轮 `avgMainThreadCpuMs = 6181.250` 的后台报告，
   当前已经明显回落，但 500ms 级主线程仍远未达到可用状态。

这说明：

1. 新的 dynamic group-slot / skinned rescue 已经真实进入 semantic core 主路径，
   不再只是 rigid rescue 兜底
2. 主线程 CPU 已经比上一轮明显回落，但仍远未可用
3. 单位旧 fallback 还没有完全归零
4. 当前主线程压力仍然集中在 `RenderQueue/MainLoop/semantic scene append` 交界热路径
5. 本轮补掉 `runObserveValidation()` 的 `UpperLayer` 空转后，
   低压场景 `avgMainThreadCpuMs` 仍然是 `670.536ms`，
   没有把主线程拉回到可用区间
6. 低压场景 `fallbackDrawCount = 3253` 反而说明：
   - 当前 object shadow ownership 仍没有真正收干净
   - 旧 fallback 的总体成本仍非常高

本轮最新 ownership/prune 验证进一步给了一个“功能与性能要分开看”的结论：

1. 新报告：
   - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_08_24_25.html`
2. 同轮后台截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_082419.png`
3. 同轮 runtime 摘要：
   - `frameCount = 2`
   - `avgMainThreadCpuMs = 13460.938`
   - `fallbackDrawCount = 474`
   - `dynamicPoseCount = 878`
   - `dynamicSkinnedOutputCount = 420`
4. 这说明：
   - 新的 strong-key fallback prune 已经真实生效，低压图里“剩余 legacy fallback work”不再停在 `3253`
   - 但本轮 report 只采到 `2` 帧，不能把这次 CPU 异常简单归因到 prune 补丁本身
5. 当前最新 section hotspot 仍然高度集中在：
   - `Hook_FlushAndReset/EndFrame`：`avgSelfCpuMs = 9767.753`
   - `Hook_WorldObjects_RenderGroup/Orig`：`avgSelfCpuMs = 1338.127`
   - `SceneCollector/RegisterBatch`：`avgSelfCpuMs = 255.817`

## 4. Best Current Diagnosis

目前最可信的技术判断是：

1. 现在已经能从 `meshData` 拿到当前帧 positions
2. 本轮已经确认：
   - `meshData + 0xE0` 不能当“index pointer”理解
   - 更像 `primitive base index / prepared primitive backing` 的起点
3. 本轮又进一步确认：
   - `meshData + 0xF0` 在当前这些 unit path 上不是 `runtimeModel*`
   - 更像 `transform / binding / aux-layer sideband id`
4. 本轮对 `layerState` 的再验证说明：
   - `batch layerStatePtr` 更像是 `MeshLayerStateRecord + 0x04` 的 state view
   - 不能继续把它或者 `layerStatePtr - 4` 当成 authoritative `MeshLayerStateRecord` 基址
   - 当前能稳定读取的 authoritative 来源仍应优先是
     `meshInfo->layerStates + layerIndex * 0x24`
5. 但单位阴影错误形态说明，仍然不能只靠：
   - source geoset indices
   - current-frame positions
6. 本轮验证进一步说明：
   - `stream1` 很可能确实承载了 skinned path 需要的 runtime group slot
   - 但当前只按“每顶点首字节 group slot”解释，仍不足以完全还原 draw contract
7. 另外，本轮日志已经明确看到：
   - 当前很多 unit rescue 仍是 `dynIdx=0`
   - `dynamic index hash/topology` 虽然已经接上宿主 key
   - 但 authoritative index slice 还没有真的被恢复出来
8. 本轮新增的 `layerStateWord1C/+0x20` pointer-candidate 路线已经真实进到代码和后台样本里，
   但当前 probe 仍然是：
   - `dynGrpSrc = -1`
   - `dynamic-group override = 0`
   这代表“把 batch state view 当 authoritative source”以及“直接从 `+0x1C/+0x20` 抽 aux pointer”
   至少在当前这批 skinned unit 样本上还没有真正命中
9. 更可能还缺：
   - `primitiveRecords -> firstIndex/indexCount/topology` 的 authoritative draw slice
   - `meshData + 0xC8/+0xCC/+0xE0` 的真实 prepared primitive contract
   - `meshData` 当前帧 index/topology source
   - `stream1` 之外的当前 draw-time auxiliary stream / binding contract
   - `transform_or_pose_ctx` / aux-layer resource binding 对 stream 的真实解释
   - 真实的 skinned dynamic geometry 语义
10. 本轮最新验证把诊断再往前推了一步：
   - 仅仅把 `draw slice` 从 `sub-primitive sequence` 里反定位回来，还不足以让单位阴影转正
   - 仅仅把 `dynamic-group override` 这条当前死分支打通，也不足以消掉“大块方影”
11. 因此当前更可信的判断是：
   - `index/topology` 的恢复仍然重要
   - 但真正没补完的很可能已经不只是一条 `baseIdx`
   - 更大的缺口仍在 `stream1 + aux-layer binding` 这套 skinned auxiliary contract
12. 本轮代码已经把一个更具体的子假设实装进去了：
    - `stream1` 可能不是“单字节 slot”
    - 而是“每顶点 packed runtime slot tuple”
13. 但本轮还没有拿到能证明“packed tuple 已把单位阴影从块状拉回正确轮廓”的最终帧证据
14. 所以当前主 blocker 仍然是：
    - `meshData dynamic geometry contract`
    - 同时伴随一个次级验证 blocker：
    - `isolated-desktop capture size / short-window perf stability still not yet trustworthy`

也就是说，当前主 blocker 是：

`meshData dynamic geometry contract is still incomplete, especially stream1/aux binding plus authoritative draw slice`

而不是：

`看不见单位`

## 5. Plan Progress Mapping

对照“语义阴影彻底切换计划”，当前大致进度是：

1. `Phase 0` 基线与止血：已完成
2. `Phase 1` control plane：已完成
3. `Phase 2` semantic contract：已完成
4. `Phase 3` ShadowRendererCore：已完成
5. `Phase 4` DXVK validation host：已进入真实接管，未完成
6. `Phase 5` native D3D9 / late inject：未完成

更准确地说，当前卡在：

`Phase 4 functional correctness of unit/skinned shadow contract`

## 6. Immediate Next Steps

下一步优先级必须是：

1. 不要再把 `meshData + 0xF0` 当 `runtimeModel*` 主假设使用：
   - 下一轮应改成按 `binding / aux-layer / transform sideband id` 去解释
2. 直接围绕 `meshData + 0xC8/+0xCC/+0xE0` 补 probe：
   - 记录 `subPrimitivePairs`
   - 对照 `primitiveRecords`
   - 把当前 unit path 的真实 `firstIndex/indexCount/topology` 钉死
3. 继续逆向 `meshData` 当前帧动态几何 contract，重点确认：
   - position stream
   - index/topology source
   - auxiliary stream/binding
   - `stream1` 是否只是 group slot，还是还需要额外 aux binding
4. 把当前 `dynIdx=0` 的 unit rescue 先推进到“能拿到真实 draw slice”，
   再继续判断阴影形态是否从方块/撕裂转正
5. 在功能正确性继续推进的同时，优先削减：
   - `runObserveValidation -> submitFrame` 之外仍残留的重复遍历
   - `War3TryAppendSemanticShadowPacket` 的 per-packet 分配/拷贝
6. 基于本轮新结论，下一轮应优先补：
   - 直接验证 `stream1 packed tuple -> dynamic runtime group palette` 是否真正命中单位主路径
   - 继续把 `meshData + 0x94` 的 `aux_layer_resource_table` 与当前 layer dispatch 绑定关系接进 core
   - `subPrimitivePairs` 命中成功率 / unique match 命中率 / dynamic-group override 命中率 的后台统计
7. 在性能侧，下一轮优先检查：
   - 新补上的 strong-key prune 为什么仍然只把低压图 `fallbackDrawCount` 收到 `474`
   - 剩余 fallback 是缺 `handle/worldObjectEntry/sceneNode/runtimeModelPtr` 身份键，还是本来就不该被 unit semantic path 接管
   - `War3AugmentShadowSemanticFromVisibleManifest()` 是否还在做无意义全量补全
8. 当前最先必须补上的验证工作不是泛泛“再跑一轮 AutoTest”，而是：
   - 修掉隔离桌面 control-plane 抓图仍落成 `1902x963` 的尺寸漂移
   - 保证 perf report 在短窗下能稳定拿到超过 `2` 帧的可比较样本
   - 然后重新用同一条后台链确认单位阴影是否从大块方影转正

## 7. Do Not Regress

后续推进时不能退回以下错误方向：

1. 不要把“单位有影子了”误判为已完成
2. 不要重新依赖旧 `VB/IB snapshot/freeze` 作为默认正确性来源
3. 不要再次把 `UpperLayer` observe 链默认打开
4. 不要要求用户频繁前台手工测试

## 8. Latest Key Artifacts

截图：

1. `AutoTest/artifacts/screenshots/war3_20260418_handle_normalized.png`
2. `AutoTest/artifacts/screenshots/war3_20260418_015607.png`
3. `AutoTest/artifacts/screenshots/war3_20260418_021249.png`
4. `AutoTest/artifacts/screenshots/war3_20260418_024614.png`
5. `AutoTest/artifacts/screenshots/war3_20260418_024800.png`

关键报告：

1. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_00_49_38.html`
2. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_01_05_00.html`
3. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_01_52_27.html`
4. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_01_56_06.html`
5. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_02_10_41.html`
6. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_02_13_01.html`
7. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_02_46_18.html`
8. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_03_10_06.html`
9. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_03_12_33.html`
10. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_06_14_57.html`
11. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_06_17_13.html`
12. 当前最新自动报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_08_24_25.html`

本轮 control plane summary-only 验证：

1. `semanticCoreResolved = 366`
2. `semanticCoreSkinnedResolved = 366`
3. `semanticCoreSkippedNoRuntimeGroupPalette = 39`
4. `visibleCount = 921`
5. `unitCount = 288`
6. `recordsWithRuntimeModel = 442`
7. `frameNumber = 882`

本轮后台验证 blocker：

1. `capture_final_frame` 在隔离桌面下仍超时，未产出新的最终帧截图
2. 本轮未生成新的 `war3_perf_report_auto_*.html`

本轮自动化结果归档：

1. `AutoTest/artifacts/automation_runs/rb_mesh_contract_1776451581.json`
2. `AutoTest/artifacts/automation_runs/rb_mesh_probe_1776451682.json`
3. 后台截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_031007.png`
   - `AutoTest/artifacts/screenshots/war3_20260418_031234.png`
4. `AutoTest/artifacts/automation_runs/rb_stateptr_candidate_1776471208.json`
5. `AutoTest/artifacts/automation_runs/rb_stateptr_probe_1776471326.json`
6. `AutoTest/artifacts/automation_runs/rb_fallback_owner_prune_1776471829.json`
7. 后台截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_081540_rb_stateptr_probe_1776471326.png`
   - `AutoTest/artifacts/screenshots/war3_20260418_082419.png`

关键研究文档：

1. `docs/plan/war3_unit_shadow_mesh_stream_probe_2026_04_17.md`
2. `docs/plan/semantic_shadow_control_plane_status_2026_04_17.md`
3. `docs/plan/upper_layer_shadow_cutover_status_2026_04_16.md`

## 9. Latest round update (2026-04-18 05:13 +08:00)

### 9.1 What landed

本轮继续沿着 `stream1/aux binding` 这个正确工作面推进，但把实现收得更窄，优先验证“是不是一直读错了 `stream1` 记录内部字段位置”：

1. `ShadowRendererCore` 的两条 `stream1` 解码 helper 都不再假设
   `offset=0`：
   - `TryResolveMeshDynamicPackedRuntimeGroups()`
   - `TryResolveMeshDynamicGroupSlots()`
2. 当前实现已经改成：
   - 先按候选 stride 扫描
   - 再在每个 stride 内扫描 `stream1` 的候选子偏移
   - 选择 sample 上最像真实 group contract 的 offset 再做全量 decode
3. 也就是说，本轮不再只把：
   - `stream1[i * stride + 0]`
   当作唯一可信入口；
   现在允许 `stream1` 的 packed tuple / group slot 落在 stride 内其他字节位置
4. 为了让后续 probe 能继续闭环，本轮还把新信息写进日志：
   - `dynamic-mesh rescue` 新增 `dynGrpOff`
   - `dynamic-group override` 新增 `offset`
5. 性能/ownership 侧本轮还补了一个低风险止血：
   - `War3TryPopulateSemanticShadowScene(unitsOnly)` 的 legacy fallback prune
   - 现在会把 `objectKind=Unknown && vertexBlendIndexed=true` 这类 unit-like fallback
     也一起清掉
   - 不再只覆盖 `vertexBlendEnabled`
   - 这样和 capture 侧 `semanticSceneOwnsUnitCapture` 的 unit-like 判定更一致
6. 本轮实际代码改动集中在：
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `src/d3d9/d3d9_device.cpp`
7. 本轮构建验证：
   - `ninja -C build32` 通过

### 9.2 Background validation

本轮继续全部走后台隔离桌面 control plane，没有占用前台：

1. automation 归档：
   - `AutoTest/artifacts/automation_runs/rb_stream1_offset_1776460332.json`
2. 最终帧截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_051212_rb_stream1_offset.png`
3. 本轮 `capture_final_frame` 在 control plane 下成功返回，不再是上轮那种隔离桌面超时
4. 本轮 control plane 摘要：
   - `semanticCoreResolved = 365`
   - `semanticCoreSkinnedResolved = 365`
   - `semanticCoreSkippedNoRuntimeGroupPalette = 39`
   - `visibleCount = 949`
   - `unitCount = 287`
   - `recordsWithRuntimeModel = 441`
   - `frameNumber = 882`
5. 但新的 perf report 仍没有翻新：
   - 当前最新 perf report 依旧停在
     `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_03_12_58.html`

### 9.3 Real conclusion of this round

这轮还不能宣称“单位阴影形状已转正”，因为：

1. 新截图里虽然 object shadow 仍然存在
2. 但地面上仍然能看到明显过宽、过硬、近似楔形/大块状的单位影子团
3. 这还不是可以签收的“按单位真实 mesh 轮廓收敛”的阴影
4. 换句话说：
   - `stream1` 解码现在已经从“固定 offset 假设”推进到“子偏移扫描”
   - `capture_final_frame` 重新恢复
   - 但单位阴影视觉正确性 blocker 仍未解除
   - perf report 落盘/翻新仍然是独立待修问题

### 9.4 Best next step after this round

基于这轮结果，下一步最该做的不是回到 VB/IB snapshot，也不是继续拍脑袋猜 `meshData + 0xF0`，而是：

1. 把 `meshData + 0x94` 的 `aux_layer_resource_table`
2. `MeshLayerDispatchRecord + 0x14/+0x18`
3. `MeshAuxResourceEntry + 0x08 = resource_binding`

这套 layer/aux binding contract 真正接进 `stream1` 解码判定里，继续确认当前 skinned unit path 到底缺的是：

1. `stream1` 内部字段 offset
2. layer-specific aux binding
3. 或者两者一起缺

## 9.5 Latest round update (2026-04-18 06:18 +08:00)

### 9.5.1 What landed

本轮继续沿着 `layer/aux binding -> stream decode` 这个方向推进，但把实现收口成“更接近原始 RenderQueue contract 的最小落地”：

1. `ShadowRenderableRecord` 现在正式携带 `layerState`：
   - 不再只带 `layerIndex`
   - 这样 semantic core 后续读取 layer-state 相关字段时，可以优先使用主队列批次里已经生成好的状态块指针
2. `ShadowRendererCore` 新增 `MeshLayerBindingContract`：
   - 会尝试把 `sceneNode + meshIndex + layerIndex + layerState + meshData + aux table`
     收成一份本轮 layer contract
   - 当前会记录：
     - `primary_resource_binding`
     - `aux_ref_index_0/1`
     - `aux_ref_enable_0/1`
     - `aux_resource_binding_0/1`
3. `TryResolveMeshDynamicPackedRuntimeGroups()` /
   `TryResolveMeshDynamicGroupSlots()` 不再只扫：
   - `stream1 ptr + {4/8/12/16}`
   当前已经补成：
   - 更宽的 stride 候选：`20/24/28/32`
   - 以及在 layer contract 判定“当前 layer 可能走 interleaved aux view”时，
     额外尝试：
     - `primary stream ptr + primary stride`
     - 在 primary stride 内继续扫 aux offset
4. 为了让这条线能被后台验证明确证伪/证实，本轮日志新增了：
   - `dynGrpPrimary`
   - `bind[p=... a0=... a1=...]`
   - 用于区分本轮 dynamic group 解码到底来自：
     - `stream1`
     - 还是 `primary stream` 上的 interleaved aux view
5. 本轮实际代码改动集中在：
   - `src/d3d9/war3/shadow/war3_shadow_runtime_contract.h`
   - `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
6. 本轮构建验证：
   - 两次 `ninja -C build32` 均通过

### 9.5.2 Background validation

本轮继续全部走隔离桌面后台 control plane，没有占用前台：

1. automation 归档：
   - `AutoTest/artifacts/automation_runs/rb_aux_contract_1776464076.json`
   - `AutoTest/artifacts/automation_runs/rb_aux_contract2_1776464212.json`
2. 最终帧截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_061436_rb_aux_contract_1776464076.bmp`
   - `AutoTest/artifacts/screenshots/war3_20260418_061652_rb_aux_contract2_1776464212.bmp`
3. 本轮 control plane 摘要：
   - 第一轮：
     - `semanticCoreResolved = 365`
     - `semanticCoreSkinnedResolved = 365`
     - `visibleCount = 940`
     - `unitCount = 287`
   - 第二轮：
     - `semanticCoreResolved = 366`
     - `semanticCoreSkinnedResolved = 366`
     - `visibleCount = 941`
     - `unitCount = 288`
4. 后台验证链的一个正向变化：
   - 新的 perf report 这轮已经再次翻新，不再停在 `03:12:58`
   - 本轮新报告：
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_06_14_57.html`
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_06_17_13.html`
5. 但功能侧日志同时给出了明确结论：
   - `dynamic-group override` 事件仍然是 `0`
   - `dynamic-mesh rescue` 仍然是：
     - `dynGrpStride=0`
     - `dynGrpOff=0`
     - `dynGrpPacked=0`
     - `dynGrpPrimary=0`
   - 代表本轮新增的 primary-stream aux 尝试虽然已经在代码里存在，
     但后台样本里还没有真正命中

### 9.5.3 Real conclusion of this round

这轮的真实结论是：

1. `layerState` 已经正式进入 semantic contract
2. `layer/aux binding` 已经不是纯文档计划，而是已经接进 core 的真实判定分支
3. `primary stream interleaved aux view` 也已经进入运行时候选集
4. 但后台验证明确说明：
   - 当前样本里这条新候选仍然没有真正命中
   - 单位阴影仍然是大块、过硬、近似楔形的错误轮廓
   - 也就是说，功能 blocker 还没有解除
5. 同时本轮日志还暴露了一个更具体的新 blocker：
   - 即使使用主队列批次里带下来的 `layerState`
   - 当前读出来的 `bind[p=... a0=... a1=...]` 仍然不够“像一个可信的最终 contract”
   - 例如：
     - 一些样本出现 `bind[p=0 a0=0/0/0 a1=449320716/0/787027760]`
     - 另一些样本出现 `bind[p=0 a0=0/0/0 a1=2/0/786922800]`
   - 这说明至少还有一项没有完全钉死：
     - `batch layerStatePtr` 是否就是 `MeshLayerStateRecord` 基址
     - 或者 `+0x1C/+0x20` 的语义并不是之前假设的简单 enable flag
     - 或者 `dispatch + aux table` 仍存在一层间接/二次索引
6. 另外，本轮新的 perf report 虽然已经恢复翻新，但性能依旧不可用：
   - `avgMainThreadCpuMs ≈ 11828`
   - `fallbackDrawCount = 238`
   - 所以“报告链恢复”不能被误判为功能转正

### 9.5.4 Best next step after this round

基于这轮代码和后台证据，下一步最该做的是：

1. 不要再把当前 `bind[p / a0 / a1]` 结果当成 authoritative layer contract
2. 直接回到 IDA / 原生提交调用点，把下面三件事钉死：
   - `RenderBatchElement::layerStatePtr` 指向的到底是不是 `MeshLayerStateRecord` 起点
   - `MeshLayerStateRecord + 0x1C/+0x20` 到底是 enable flag、binding token，还是别的派生状态
   - `MeshLayerDispatchRecord + 0x14/+0x18 -> MeshAuxResourceEntry + 0x08`
     中间是否还有一层 caller-side 间接
3. 下一轮最好新增一层更贴近原生现场的 probe：
   - 直接打印 `layerStatePtr` 前若干 DWORD
   - 同时对照当前 `RenderQueue_ApplyDrawStateAndSamplerPair` 实参
   - 这样才能判断“我们现在读错的是基址、字段，还是整个 contract 层次”
4. 在功能侧，当前最值得继续盯的不是“有没有新的 dynamic-group override 统计”，而是：
   - 为什么这些单位样本仍然全部落成 `dynGrpStride=0`
   - 以及 `geoIdx=3 / geoIdx=2` 这类样本上 `baseIdx` 仍然是离谱大值
   - 这两条仍然指向：
     - `layer/aux binding contract`
     - 与 `authoritative index/topology slice`
     两条线都还没真正钉死

## 9.6 Latest round update (2026-04-18 08:26 +08:00)

### 9.6.1 What landed

本轮一共落了两类补丁：一类继续验证 `layerState` / aux binding contract，另一类直接收缩 semantic 已经接管过的 legacy fallback 工作量。

1. `ShadowRendererCore::TryResolveMeshLayerBindingContract()` 不再把：
   - `batch layerStatePtr`
   - 或 `layerStatePtr - 4`
   当成 authoritative `MeshLayerStateRecord` 基址；
   现在只把它们保留为诊断参考，真正读取 contract 时优先使用
   `meshInfo->layerStates + layerIndex * 0x24`
2. `ShadowRendererCore` 同轮新增了额外 dynamic aux candidate：
   - `layerStateWord1C`
   - `layerStateWord20`
   并把 candidate source 写进日志：
   - `dynGrpSrc`
   用于区分 dynamic-group decode 最终来自：
   - `stream1`
   - `primary stream`
   - `aux entry`
   - 还是 `layerState`
3. `War3ShadowFallbackDraw` 现在正式携带：
   - `normalizedHandle`
   - `worldObjectEntry`
   - `sceneNode`
   - `runtimeModelPtr`
   - `modelKey`
4. `War3TryPopulateSemanticShadowScene(unitsOnly)` 的 legacy fallback prune
   已从“只看 normalized handle”扩成“强身份键集合”：
   - `normalizedHandle`
   - `worldObjectEntry`
   - `sceneNode`
   - `runtimeModelPtr`
   这样 semantic submit 过的 unit-like object，不再因为 `handle=0` 或 batch handle 漏传而继续保留旧 fallback work
5. `fallbackDrawCount` 也同步改成：
   - 直接等于当前 `shadowFallbacks.size()`
   - 不再只是累计 capture 次数
6. 本轮实际代码改动集中在：
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `src/d3d9/d3d9_war3_scene.h`
   - `src/d3d9/d3d9_device.cpp`
7. 本轮构建验证：
   - `ninja -C build32` 通过

### 9.6.2 Background validation

本轮继续全部走隔离桌面后台验证，没有占用前台：

1. `layerState` / state-view probe：
   - `AutoTest/artifacts/automation_runs/rb_stateptr_candidate_1776471208.json`
   - `AutoTest/artifacts/automation_runs/rb_stateptr_probe_1776471326.json`
   - 截图：`AutoTest/artifacts/screenshots/war3_20260418_081540_rb_stateptr_probe_1776471326.png`
   - 报告：
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_08_13_50.html`
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_08_15_47.html`
2. 这轮 state-view probe 的明确结论：
   - `dynamic-mesh rescue = 11`
   - `runtime-group miss = 11`
   - `dynamic-group override = 0`
   - `dynGrpSrc = -1`
   说明新的 `layerStateWord1C/+0x20` candidate 虽然已经进代码和日志，但当前样本里没有真正命中 dynamic-group 还原
3. fallback ownership/prune 验证：
   - `AutoTest/artifacts/automation_runs/rb_fallback_owner_prune_1776471829.json`
   - 截图：`AutoTest/artifacts/screenshots/war3_20260418_082419.png`
   - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_08_24_25.html`
4. 这轮最新报告显示：
   - `frameCount = 2`
   - `avgFps = 0.073`
   - `avgFrameTimeMs = 13786.073`
   - `avgMainThreadCpuMs = 13460.938`
   - `fallbackDrawCount = 474`
   - `dynamicPoseCount = 878`
   - `dynamicSkinnedOutputCount = 420`
5. 同轮 section hotspot：
   - `Hook_FlushAndReset/EndFrame`：`avgSelfCpuMs = 9767.753`
   - 嵌套 `Hook_FlushAndReset/EndFrame`：`avgSelfCpuMs = 2203.158`
   - `Hook_WorldObjects_RenderGroup/Orig`：`avgSelfCpuMs = 1338.127`
   - `SceneCollector/RegisterBatch`：`avgSelfCpuMs = 255.817`
6. control-plane 抓图链本轮是成功的，但截图尺寸仍然只有：
   - `1902x963`
   还没有恢复到 `2560x1440` baseline

### 9.6.3 Real conclusion of this round

这轮的真实结论要分成功和未完成两部分看：

1. 成功部分：
   - semantic ownership -> legacy fallback prune 的同构补丁已经真实生效
   - 低压场景里 `fallbackDrawCount` 已经不再停在前一轮的 `3253`
   - 当前最新低压样本里已经收到了 `474`
2. 但这不等于功能完成，因为：
   - 最新截图里单位阴影仍然是过宽、过硬、近似楔形/大块状的错误投影
   - 视觉 blocker 依然是 `meshData dynamic geometry contract`
   - 不是“单位看不见”
3. `layerState` / state-view 这条 probe 路线目前也只能算“被证实已落地，但尚未命中”：
   - `dynGrpSrc = -1`
   - `dynamic-group override = 0`
   - 所以当前并不能把 `batch layerStatePtr` 或 `+0x1C/+0x20` 路线当成已经钉死的 authoritative source
4. 性能侧本轮不能简单报“退化”或者“优化成功”：
   - 新报告只采到了 `2` 帧
   - 这说明 report 已经翻新，但短窗样本还不够稳定
   - 当前只能确认热点仍然主要集中在：
     - `EndFrame`
     - `WorldObjects_RenderGroup`
     - `SceneCollector/RegisterBatch`

### 9.6.4 Best next step after this round

基于本轮代码和后台证据，下一轮最该做的是：

1. 继续向 IDA / 原生调用点收口，明确确认：
   - `RenderBatchElement::layerStatePtr` 的真实基址语义
   - `MeshLayerStateRecord + 0x1C/+0x20` 的真实字段含义
2. 保留这轮新的 strong-key prune，不要回退；
   下一轮应直接分析剩余 `474` 个 fallback draw 是：
   - 缺少强身份键
   - 还是本来就不是 unit semantic path 应该接管的对象
3. 不要用 `modelKey` 单独做 prune key：
   - 它现在只适合作为诊断上下文
   - 不适合作为多实例共享模型下的唯一删除条件
4. 验证链侧下一步优先级已经变成：
   - 修掉隔离桌面截图尺寸漂移
   - 让 perf report 稳定拿到超过 `2` 帧的窗口
   - 然后再拿同一条后台链复核单位阴影是否真的从“大块方影”收敛到真实 mesh 轮廓

## 9.7 Latest round update (2026-04-18 09:32 +08:00)

### 9.7.1 What landed

本轮没有继续在“死路径”上猜字段，而是先把 `dispatch contract probe` 真正搬到了当前 live path 上：

1. 先在 `Hook_FlushSortedItems` 入口补了低频 `DXVK SemanticDispatchGate`：
   - 用于确认当前到底有没有走 `reimpl::RenderQueue::FlushSortedItems_StdSort`
   - 以及当前是 `takeover` 还是 `fallback`
2. 随后把真正的 `DXVK SemanticDispatchProbe / Skip` 从
   `war3_render_queue.cpp` 的未生效副本，平移到了：
   - `Hook_RenderQueue_Dispatch_Common`
   - `Hook_RenderQueue_Dispatch_Special`
3. 现在即使 `QueueTakeover` 关闭，原生 flush 经过 dispatch hook 时，
   仍然能拿到：
   - `renderablePart`
   - `VisibleRenderableRegistry` 里的 `sceneNode / meshData / layerState`
   - 以及同帧 authoritative `meshInfo->layerStates + layerIndex * 0x24`
4. 为了避免后续再被“改了 `.cpp` 但没进 DLL”误导，本轮还同步清掉了
   `war3_render_queue.cpp` 那份死改动，只保留真正参与构建的 header /
   hook path 版本。
5. 本轮构建验证：
   - `ninja -C build32` 通过
   - 后台部署的新 DLL 时间戳已刷新到
     `2026-04-18 09:31:32 +08:00`

### 9.7.2 Background validation

本轮继续全部走隔离桌面后台 AutoTest / DBWIN，没有占用前台：

1. gate 验证：
   - `AutoTest/artifacts/automation_runs/rb_dispatch_gate_1776475765.json`
2. live dispatch probe 验证：
   - `AutoTest/artifacts/automation_runs/rb_dispatch_live_1776475910.json`
3. 最新 perf report：
   - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_09_31_59.html`
4. control-plane 摘要仍然稳定：
   - `semanticCoreResolved = 366`
   - `semanticCoreSkinnedResolved = 366`
   - `visibleRenderableCount = 921`
   - `visibleRenderableMainCount = 411`
   - `visibleRenderableTransparentCount = 510`
5. `DXVK SemanticDispatchGate` 已明确给出：
   - `phase=fallback`
   - `mode=0`
   - `reason=1`
   - 对照 `War3QueueTakeoverReason`，即：
   - `reason=Disabled`
6. 这说明：
   - 当前 `QueueTakeover` 在真实运行里是关闭的
   - 所以前一轮挂在 `reimpl FlushSortedItems` 里的 probe 天然不会命中
7. dispatch hook 版本的 probe 则已经真正打出来：
   - `SemanticDispatchProbe count = 15`
   - `SemanticDispatchSkip count = 35`
   - `SemanticDispatchSkip` 原因分布：
     - `no-runtime-model = 33`
     - `non-unit-kind = 2`
8. probe 命中的 15 个 unit-like runtime-model 样本里，本轮拿到了三条稳定结论：
   - `dRec = 4` / `dView = 0` 全部一致
   - 也就是 `batchStatePtr == canonicalLayerStateRecord + 4`
   - 继续坐实“batch ptr 是 state-view，不是 record base”
9. probe 命中的 `primary` 字段也很稳定：
   - `primary stride / arg0` 总是相等
   - 样本分布为 `52/52`, `18/18`, `49/49`, `21/21`, `88/88`
10. 但 `stream1 stride` 在同批 probe 里仍然呈现明显离谱的大值：
    - 例如 `738799824`, `738143720`, `738593096`
    - 这再次说明当前 `meshData + Stream1Stride` 解释不能直接当 authoritative stride
11. 本轮 probe 里 `bind` 也给出了一个新的强证据：
    - `primaryResourceBinding` 几乎都稳定是 `2`
    - `stageMode0/1` 全部是 `10/10`
    - 但当前被当成 `auxRefEnable0` 的值，在 15 个 probe 样本里稳定是
      `0x1A7F2400`
    - 这显然不像布尔 enable flag，更像指针/token

### 9.7.3 Real conclusion of this round

这轮最重要的真实结论不是“又多了几条日志”，而是 blocker 被进一步缩窄了：

1. `QueueTakeover` 当前就是关闭的：
   - 所以不能再把 `reimpl FlushSortedItems caller-site` 当成本轮 authoritative probe 入口
2. 当前 authoritative caller-side probe 入口已经切到：
   - `Hook_RenderQueue_Dispatch_Common/Special`
   - 这条路径在当前真实运行里是活的
3. `batch layerStatePtr = canonical record + 4` 现在不只是推断，
   而是被 dispatch 现场再次稳定验证
4. 当前 `MeshLayerStateRecord + 0x1C/+0x20` 的旧解释明显不成立：
   - 至少 `+0x1C` 读出来的不是“0/1 enable flag”
   - 更像某种指针 / binding token / sideband pointer
5. 同时：
   - `MeshAuxResourceEntry + 0x08`
   - `MeshData::Stream1Stride`
   也仍然不像当前代码里理解的“最终 binding id / 可信 stride”
6. 换句话说，本轮把 blocker 从：
   - “probe 没命中，不知道是路径死了还是字段错了”
   收缩到了：
   - “dispatch 现场已经拿到，但 `layer/aux` 某几段字段语义仍然读错”
7. 功能侧仍未完成：
   - `dynamic-mesh rescue` 仍然是 `dynGrpSrc = -1`
   - `dynSkin = 0`
   - 单位阴影形状错误本轮没有转正

### 9.7.4 Best next step after this round

基于本轮代码和后台证据，下一轮最该做的是：

1. 不要再把 `MeshLayerStateRecord + 0x1C/+0x20` 当布尔 enable flag 使用；
   先按“非零 pointer/token”重新审视它们在 dynamic aux candidate 里的角色
2. 直接对照本轮 `SemanticDispatchProbe` 命中的 handle/rawcode，
   去匹配同批 `dynamic-mesh rescue` 的 miss 样本，确认：
   - `MeshAuxResourceEntry + 0x00` 更像 stride/count/slot-count 的哪一种
   - `MeshAuxResourceEntry + 0x08` 到底是 stream ptr、binding token，还是别的间接层
3. 在进入下一轮字段重解释前，不要回退本轮新的 dispatch hook probe；
   它现在是当前最接近真实调用现场的 authoritative 证据入口
4. `SemanticDispatchGate` 可以继续暂留一轮：
   - 用来提醒后续线程“当前 QueueTakeover 仍是 Disabled”
   - 避免再把时间花在不会执行的 reimpl caller-site 上

## 9.8 Latest round update (2026-04-18 10:14 +08:00)

### 9.8.1 What landed

本轮围绕 `meshData dynamic geometry + aux contract` 落了两类高置信度补丁：

1. `ShadowRendererCore` 不再把 `MeshAuxResourceEntry + 0x08` 继续记成
   “binding id”：
   - 已改成显式 `aux stream ptr`
   - 并在 layer contract 里固定保留 `aux stream stride = 8`
   - 对应 `RenderQueue_ApplyDrawStateAndSamplerPair -> sub_6F0E35B0`
     的最新 IDA 结论
2. `prefersPrimaryStreamAuxView()` 已收窄：
   - 不再因为“有 aux gate”就去盲扫 `primary stream`
   - 现在只在 `primaryResourceBinding == 3` 的 prepared-primitive profile
     下把 primary interleaved view 当候选
3. `dynamic-mesh` 的 aux 候选顺序已改成：
   - 先尊重 `aux stream ptr`
   - 再考虑其它 stream/layerState 候选
4. `TryResolveMeshDynamicIndexStream()` 新增了
   `primitiveBaseIndex` 垃圾值止血：
   - 当 `meshData + 0xE0` 明显越界
   - 但当前 slice count / 单 primitive record 仍可信时
   - 允许保守回退到 `baseIdx = 0`
   - 避免整条 dynamic index contract 直接掉成 `dynIdx=0`
5. 为了让下一轮不再靠猜，本轮还给动态日志补了：
   - `head0/head1`
   - 即 aux stream 前 8 字节采样
   - 用于直接观察当前 8-byte aux 流头部到底长什么样
6. 本轮实际代码改动集中在：
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
7. 本轮构建验证：
   - `ninja -C build32` 通过

### 9.8.2 Background validation

本轮继续全部走后台隔离桌面，没有占用前台：

1. 先做了一轮短窗 `run_quick_autotest`：
   - 场景：`dynamic_shadow_pressure`
   - 结果：可正常启动、进入游戏、静默结束
   - 最新报告：
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_10_12_54.html`
2. 随后启动了单轮后台任务并在运行中抓 DBWIN：
   - `start_periodic_perf_test(jobId=1, rounds=1, sample=8s)`
   - 运行期成功抓到新的 `dynamic-mesh rescue` 日志
3. 本轮新的关键 live 日志结论：
   - 以前 `geoIdx=3` 这类样本经常是：
     - `baseIdx=577623248`
     - `dynIdx=0`
   - 现在已变成：
     - `baseIdx=0`
     - `dynIdx=36`
   - 代表这轮 `base=0` slice 止血已经真实命中 live path
4. 其它 live 样本也同步变成了更可信的 index 状态：
   - `geoIdx=0`：`dynIdx=1536`, `baseIdx=0`
   - `geoIdx=1`：`dynIdx=183`, `baseIdx=0`
   - `geoIdx=2`：`dynIdx=12`, `baseIdx=0`
5. 但 dynamic group 侧仍然没有转正：
   - `dynGrpSrc = -1`
   - `dynGrpStride = 0`
   - `dynGrpOff = 0`
   - `dynSkin = 0`
   - `dynamic-group override` 事件数仍为 `0`
6. 新增的 aux stream 头采样已经把 blocker 再收紧了一层：
   - 例如：
     - `head0=1:3F775D79/3EB5579B`
     - `head0=1:3EE8F6CF/3F48929B`
     - `head0=1:3DED5843/3F332032`
   - 这些值明显更像 `float` 权重/系数，而不是小范围 group slot / packed tuple
7. 本轮后台单轮任务产物：
   - 报告：
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_10_14_04.html`
   - 结果摘要：
     - `avgFps = 0.087`
     - `avgFrameTimeMs = 11538.541`
     - `avgTrackedActiveCpuMs = 11455.667`
     - 截图尺寸：`2578x1398`，仍与 `2560x1440` baseline 不一致

### 9.8.3 Real conclusion of this round

这轮的真实结论是：

1. `MeshAuxResourceEntry + 0x08` 的当前语义已经可以从
   “猜测 binding id”收口成：
   - live path 上真正送进 `35B0` 的 `8-byte aux stream ptr`
2. `primitiveBaseIndex` 垃圾大值确实是当前 unit path 的真实问题之一：
   - 这轮止血后，至少一批此前 `dynIdx=0` 的样本已经恢复出非零 draw slice
3. 但 `aux stream` 本身并不是“直接可扫出来的 group slot byte array”：
   - 新的 `head0/head1` 明显更像浮点权重/系数
   - 不是之前那种“继续在 aux/stream1 上扫首字节或 4-byte tuple”
4. 也就是说，本轮把 blocker 从：
   - “aux entry +8 到底是不是指针、index slice 为什么全丢”
   收缩成了：
   - “index slice 止住了一部分，但 8-byte aux 流更像权重流，仍缺 compact bone index / one-more-hop contract”
5. 功能仍未完成：
   - 单位阴影形状本轮没有拿到可签收的视觉转正证据
   - `dynSkin` 仍然是 `0`
   - `dynamic-group override` 仍未命中
6. 性能也仍未完成：
   - 新报告窗口仍处于极端高 CPU / 低 FPS
   - 不能把本轮当成性能优化完成

### 9.8.4 Best next step after this round

基于本轮代码和后台证据，下一轮最该做的是：

1. 不要再把 `aux stream` 当“小范围 group slot 字节流”扫描主假设；
   当前更应该把它当：
   - `8-byte weight/sideband stream`
2. 直接围绕这条新证据补下一层 contract：
   - `8-byte aux stream` 哪几个字节是权重
   - compact bone index/slot 是否来自另一半字节、另一层指针，或 `stream1` 的配对视图
3. 保留本轮 `baseIdx=0` 止血，不要回退；
   它已经真实把部分 `dynIdx=0` 样本救回来了
4. 下一轮日志应优先补：
   - aux stream 第 2/3 个顶点的 8-byte 采样
   - 与同 mesh 的 `stream1` 头部并排对照
   - 这样才能判断当前缺的是：
     - `weights + compact indices` 双流配对
     - 还是 `aux ptr` 之后还有一层间接

## 9.9 Latest round update (2026-04-18 11:16 +08:00)

### 9.9.1 What landed

本轮继续围绕 `meshData aux/group contract` 做了两类低风险但高信息增量的实装：

1. `ShadowRendererCore` 新增统一的 `CollectDynamicAuxStreamCandidates()`：
   - 不再在 `TryResolveMeshDynamicPackedRuntimeGroups()` /
     `TryResolveMeshDynamicGroupSlots()` 里各自重复拼 candidate
   - 现在统一收口：
     - `stream1`
     - `primary stream`（仅 `primaryResourceBinding == 3`）
     - `MeshLayerStateRecord + 0x1C/+0x20`
     - `MeshAuxResourceEntry + 0x08`
2. `layerState` / `aux` 的 pointer-candidate 不再只试一层：
   - 当前已经把 `one-more-hop` 候选扩到二级
   - 会读取 `layerStateWord1C/20` 或 `aux stream ptr` 指向对象的前 3 个 DWORD
   - 如果其中仍出现 pointer-like 值，会继续把下一层指针送进 dynamic group candidate 集
3. `dynamic-mesh rescue` 日志新增并排样本：
   - `stream1` 前 3 个 DWORD
   - `aux0` 前 3 个 `8-byte` 样本
   - `l1c/l20` 目标前 3 个 DWORD
   - 用于直接判断当前到底更像：
     - `weights + compact indices` 双流
     - 还是 `one-more-hop` remap/span record
4. 本轮代码改动仍只集中在：
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`

### 9.9.2 Background validation

本轮继续全部走后台隔离桌面，没有占前台：

1. 两次 `ninja -C build32` 都通过
2. 第一轮 `run_quick_autotest`：
   - 工件：
     - `AutoTest/artifacts/automation_runs/rb_aux_pair_probe_1776482012.json`
   - 报告：
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_11_13_55.html`
   - 结果：
     - 新增 `stream1=` / `aux0v=` 字段全部真实命中 live log
     - 但 `capture_final_frame` 仍超时，window fallback 也因 `MainWindowHandle=0` 失败
3. 第二轮改成“手动 launch + 延迟 12s 再抓图”：
   - 工件：
     - `AutoTest/artifacts/automation_runs/rb_aux_manual_capture_1776482174.json`
   - 最终帧：
     - `AutoTest/artifacts/screenshots/war3_manual_1776482174.png`
     - control plane capture 成功，耗时 `4.749s`
     - 尺寸仍是 `1902x963`
   - 报告：
     - `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_11_16_40.html`
4. 本轮新的代表性 live 样本：
   - `geoIdx=2`：
     - `stream1=02000202/02000200/01030300`
     - `aux0v=3EBBC72E/3EF21F4B | 3E9ABCF9/3EEC1616 | 3EDC3761/3EDA643D`
     - `l1c=6A414BA0/000001DD/00000001`
   - `geoIdx=0`：
     - `stream1=24141313/25242525/24242424`
     - `aux0v=3DED5843/3F332032 | 3A28805F/3F3304AB | 3D6A71A9/3F4AC7B9`
     - `l1c=6A414BA0/0000002B/00000001`
   - `geoIdx=3`：
     - `stream1=00000000/00000000/00000000`
     - `aux0v=3EE8F6CF/3F48929B | 3E65931D/3F106306 | 3E300000/3F520008`
     - `l1c=6A414BA0/0000001A/00000001`
5. 但结果仍然是：
   - `dynGrpSrc = -1`
   - `dynSkin = 0`
   - `dynamic-group override` 事件数 = `0`

### 9.9.3 Real conclusion of this round

这轮把 blocker 又往前收了一步：

1. `stream1` 和 `aux0` 现在已经在同一条 `dynamic-mesh rescue` 样本里被并排抓到：
   - `stream1` 明显更像 compact index / slot byte tuple
   - `aux0` 明显更像 `8-byte float sideband / weight stream`
   - 因而“`weights + compact indices` 双流”假设变强了
2. `MeshLayerStateRecord + 0x1C` 也已经不再只是“pointer-like token”：
   - 它指向的对象头部第一个 DWORD 又是 pointer-like 值 `0x6A414BA0`
   - 后两个 DWORD 则更像 span/count/flag（如 `0x1A / 0x2B / 0x1DD`, `0x1`）
   - 这说明当前确实存在 `one-more-hop` contract
3. 即使把 indirect candidate 扩到二级之后：
   - dynamic group 仍未被真正解码
   - 说明现在的真实 blocker 已经不是“有没有 one-more-hop”
   - 而是“`0x6A414BA0` 这一层到底是不是 compact remap/span table，以及它如何与 `stream1 + aux0` 配对”
4. 截图链本轮也得到一个更真实的结论：
   - `run_quick_autotest` 的固定节奏下，内部最终帧截图仍可能超时
   - 但延迟后再次调用 `capture_final_frame` 可以成功
   - 因此当前截图链属于“时机敏感，但并未彻底失效”
5. 本轮仍不能宣布单位阴影转正：
   - 虽然最终帧再次成功导出
   - 但尺寸仍是 `1902x963`，不是 `2560x1440` baseline
   - 本轮重点也在 contract 继续收敛，不足以签收“阴影形状已正确”
6. 性能仍然非常差：
   - `avgFps = 0.060`
   - `avgFrameTimeMs = 16716.281`
   - `avgTrackedActiveCpuMs = 16632.581`
   - `fallbackDrawCount = 238`
   - `semanticCoreResolved = 365`
   - `semanticCoreSkinnedResolved = 365`

### 9.9.4 Best next step after this round

基于本轮代码和后台证据，下一轮最该做的是：

1. 不要再只把 `l1c` 当“pointer token”打印出来；
   应该直接补 `l1cHop` 的下一层窗口采样，确认：
   - `0x6A414BA0` 指向的是 byte tuple、pointer table，还是 span descriptor
2. 尝试做一个更专门的 pairing decoder：
   - `stream1` 作为 compact bone/group indices
   - `aux0` 作为 `8-byte` weight sideband
   - `l1cHop` 作为 remap/span table
   - 三者一起还原当前 draw 的 skinned contract
3. 保留本轮的二级 indirect candidate，不要回退；
   它已经把 blocker 从“是否存在一层间接”推进到了“下一层 record 的真实语义”
4. 后台验证方面保留“延迟后再抓最终帧”的流程：
   - 它已经证明 `capture_final_frame` 仍可成功
   - 后续需要在这个成功路径上继续观察阴影是否从方块/撕裂转正

## 9.10 Latest round update (2026-04-18 11:46 +08:00)

### 9.10.1 What landed

本轮已经把项目从“semantic validation 自己拖死 Flush 热路径”的阶段，推进到了“剩余热点重新回到 native world render/queue”的阶段：

1. `War3Renderer::EndFrame()` 现在不再在每次 `FlushAndReset` 上无条件重建 semantic frame：
   - 先按 `visible frame + visible/resource/pose count` 做节流
   - `captureLiveState()` 只在 contract 真的变化时重发
2. `ShadowValidationRuntime` 已从 `Hook_FlushAndReset/EndFrame` 热路径里摘出：
   - 不再在每个 flush 上重复 `runObserveValidation()`
   - 改成 `BeforeUi -> War3TryPopulateSemanticShadowScene()` 前按需 `ensureLatestFrameBuilt()`
3. `dynamic rescue` 新增了“静态 `vertex_group_indices` 直接吃 pose palette”的 skinned 提交路径：
   - 不再因为没解出 dynamic group override 就一律降成 rigid
4. `capture_final_frame` 这轮再次恢复到了稳定成功路径：
   - 最终帧：`AutoTest/artifacts/screenshots/war3_20260418_114548.png`
   - control-plane 截帧耗时约 `2.0s`

### 9.10.2 Background validation

本轮仍然全部走后台隔离桌面，没有占前台：

1. `ninja -C build32` 多次通过
2. 中间诊断样本：
   - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_11_43_42.html`
   - 结论：
     - `War3Renderer/EndFrame/RunObserveValidation` 仍是主 CPU 热点
3. 结构调整后的主样本：
   - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_11_45_47.html`
   - 最终帧：`AutoTest/artifacts/screenshots/war3_20260418_114548.png`
   - 结果：
     - `frameCount = 3`
     - `avgFps = 0.241`
     - `avgMainThreadCpuMs = 3645.833`
     - `avgTrackedActiveCpuMs = 4083.923`
     - `semanticSceneSubmitted = 753`
     - `semanticSceneSubmittedUnit = 753`
     - `semanticSceneSubmittedSkinned = 63`
     - `dynamicSkinnedOutputCount = 632`
     - `fallbackDrawCount = 714`

### 9.10.3 Real conclusion of this round

这轮有两个关键结论已经坐实：

1. semantic frame 构建从 `FlushAndReset` 热路径摘掉以后，性能明显改善：
   - 旧样本约 `avgFps = 0.073`
   - 新样本已到 `avgFps = 0.241`
   - `avgMainThreadCpuMs` 从 `12000+ ms` 掉到了 `3600+ ms`
2. `Hook_FlushAndReset/EndFrame` 已经不再是当前最大热点：
   - 新报告里它已经降到个位数毫秒量级
   - 剩余主热点重新变成：
     - `Hook_WorldRenderScene/Orig`
     - `Hook_FlushAndReset/Orig`
     - `Hook_WorldDispatch/Orig`
     - `Hook_WorldObjects_RenderGroup/Orig`
3. semantic 单位提交也继续抬高了：
   - `semanticSceneSubmittedSkinned` 从早先的 `21` 抬到 `63`
   - `dynamicSkinnedOutputCount` 进一步抬高到 `632`
4. 这说明当前项目已经离开“semantic contract 自己把宿主拖死”的阶段，
   进入了新的阶段：
   - 单位阴影已可见
   - semantic skinned 提交在继续提升
   - 但剩余性能瓶颈主要位于 native world render / queue hot path
5. 这轮还不能宣布完成：
   - FPS 仍然极低
   - 单位阴影虽然已经出现，但是否已经完全动态正确，还需要继续针对最终视觉确认

### 9.10.4 Best next step after this round

基于这轮代码和后台证据，下一轮最该做的是：

1. 不要再把主精力放在 `War3Renderer::EndFrame` 上；
   这条链已经被有效止血
2. 直接转向 native world render / queue 热段：
   - `Hook_WorldRenderScene/Orig`
   - `Hook_FlushSortedItems`
   - `Hook_Dispatch_Special`
   - `SceneCollector/RegisterBatch`
3. 优先收 `WorldObjects_RenderGroup -> FlushSortedItems -> DispatchSpecial/Common`
   这一段的重复工作和过量桥接
4. 保留当前 `BeforeUi` 按需 semantic build 方案，不要回退到 `FlushAndReset` 上重建 frame

## 9.10.5 Latest round update (2026-04-18 13:55 +08:00)

### 9.10.5.1 What landed

本轮继续沿着“单位阴影变动态 + 收主线程 CPU”双目标推进，已经落地并保留的稳定改动有：

1. `RenderObjectRegistry -> shadow runtime registries` 改成 batch feed：
   - `registerWorldObjectsBatch()` 不再逐对象反复桥接
   - `ModelInstanceRegistry / ShadowObjectRegistry` 新增 batch 接口与 locked merge 路径
2. `SceneCollector` 的 unit metadata 读取改成 fast path：
   - 不再对每个 `unitPtr` 都走 `UnitWrapper::IsValid/GetRawcode/GetFlags5C`
   - 改为 `SafeReadU32Fast + flags5C -> ObjectKind` 直接推断
3. `Dispatch_Common / Dispatch_Special` 的 semantic contract probe 已默认关闭，
   并补上 world-tag fast path：
   - 不再为 `shadowSemanticOnly + world-group tag 已知` 的场景重复做
     `RenderQueueTracker::GetTagStage()`
4. `ComputeShadowRuntimeBridgeTracking()` 已改成 semantic-scene-aware：
   - units-first semantic scene 已接管且 runtime chain warm 时
   - 不再维持几十帧的 identity/fallback warmup
   - legacy fallback 只保留 repair/diagnostic 角色
5. semantic scene 的动态更新签名已补齐：
   - 对 `usesDynamicMeshPositions` 的 packet
   - 动态签名不再只看 world matrix
   - 现在会把 `poseHash + dynamic geometry hash + dynamic primitive base/index hash`
     一起并入 `dynamicPoseSignature`
   - 目的是避免单位动画变化时，shadow map 仍把这类 packet 当静态 rigid 复用
6. `Hook_WorldObjects_RenderGroup` 新增了 path-blocker-only 收缩：
   - 在 units-first semantic scene 已接管时
   - 不再让旧的 PathBlocker-only 强制追踪长期拖着对象收集热路径

### 9.10.5.2 Background validation

本轮验证继续全部走后台隔离桌面，没有占前台。保留为当前稳定基线的报告是：

1. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_13_26_33.html`
   - `avgFps = 0.274`
   - `avgMainThreadCpuMs = 3317.708`
   - `SceneCollector/RegisterBatch ~= 98.861 ms`
   - `SceneCollector/CollectWorldObjects ~= 114.913 ms`
   - `Hook_Dispatch_Common ~= 2.270 ms`
   - `Hook_Dispatch_Special ~= 2.182 ms`
2. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_13_39_16.html`
   - `avgFps = 0.288`
   - `avgMainThreadCpuMs = 3192.708`
   - `SceneCollector/RegisterBatch ~= 84.971 ms`
   - `SceneCollector/CollectWorldObjects ~= 100.512 ms`
3. `E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_13_40_38.html`
   - 更长窗口验证了 runtime bridge 在 warm 状态下继续工作
   - `avgFps = 0.324`
   - `avgMainThreadCpuMs = 2990.234`
   - `SceneCollector/RegisterBatch ~= 30.890 ms`
   - `SceneCollector/CollectWorldObjects ~= 36.443 ms`
4. 最新后台截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_134039.png`
   - 单位阴影仍可见，没有回退到“完全消失”

另外说明：

1. 本轮还试过“让 dynamic packet 直接进入 persistent geometry cache”的实验
2. 它一度把 `avgFps` 继续抬高到 `0.334`
3. 但同时导致：
   - `semanticBridgeMiss` 明显反弹
   - `fallbackDrawCount` 明显反弹
   - `staticPersistentCount` 异常抬高
4. 这条实验路线已经撤回，不属于当前保留状态

### 9.10.5.3 Real conclusion of this round

这轮已经把项目推进到了一个更明确的新阶段：

1. `Dispatch_Common / Dispatch_Special` 不再是主 CPU killer
2. `SceneCollector/RegisterBatch` 和 `CollectWorldObjects` 已经被连续压低：
   - 从三位数毫秒量级压到了几十毫秒量级
3. 当前最大热点重新明确落回：
   - `Hook_WorldObjects_RenderGroup/Orig`
   - `Hook_WorldDispatch/Orig`
   - `Hook_WorldRenderScene/Orig`
4. 也就是说：
   - 我们自己的 semantic bridge / dispatch glue 已经基本止血
   - 剩余大头更像“native world render hot section + 仍存在的 legacy coexistence 成本”
5. 单位阴影方面：
   - 单位阴影仍可见
   - semantic scene 仍持续提交单位 packet
   - 动态签名已经补上，不再把 `dynamic mesh` 一律按静态 rigid 更新判断
6. 但这轮还不能宣布“单位阴影已经完全动态正确”：
   - 从最终截图看，阴影虽然存在，但还没有充分证据证明动作级动态已经完全转正
   - 后续仍要做多时刻抓图/差分，或者继续提高真正 skinned authoritative 提交占比

### 9.10.5.4 Best next step after this round

基于这轮代码和后台证据，下一轮最该做的是：

1. 不再把主要精力放在 `SceneCollector` 的小修小补上；
   它已经被明显压下去了
2. 直接围绕 `Hook_WorldObjects_RenderGroup/Orig` / `Hook_WorldDispatch/Orig`
   继续查 native hot section 与 legacy coexistence
3. 优先验证两件事：
   - semantic scene 是否还能进一步扩大“真正 skinned authoritative”覆盖
   - 当前剩余 CPU 是否主要花在 legacy fallback coexistence，而不是 semantic build
4. 动态正确性上，优先做“同场景多时刻截图差分”，验证：
   - 单位动作变化时阴影是否真的跟着变
   - 还是只是“能显示，但仍以静态/半静态形态复用”

## 9.10.6 Latest round update (2026-04-18 18:35 +08:00)

### 9.10.6.1 What landed

本轮继续围绕“单位阴影看起来是静态的”和“FPS 仍然极低”两条主线推进，已经落地并保留的改动有：

1. `ShadowRendererCore` 修掉了一个会把 unit-like packet 过早降成 rigid 的退化点：
   - 之前 `preferDynamicMeshRescue` 只要命中 `dynamicMeshRescue` 就会直接返回
   - 即使它最后只解出 `Rigid` 包，也会提前吃掉后面的
     `static geoset + runtime group palette` 正式 skinned 路线
   - 现在改成：
     - 只有 `dynamicMeshRescue` 真正解出 `Skinned` 时才提前返回
     - 如果它只能给出 `Rigid`，就继续尝试 runtime-group / pose palette authoritative 路线
2. `VisibleRenderableRegistry` 做了一轮热路径减冗余：
   - 已经有稳定 `identity + sceneNode` 的 record 不再逐条重做完整 identity resolve
   - 新增 `sceneNode -> model metadata` 的本帧缓存
   - 同一 `sceneNode` 的 `runtimeModel/modelResource/modelKey` 不再在同一帧重复解析
3. `kNativeRenderIdentityBridgeEnabled=true` 继续保留：
   - 这条线已经被后台验证为必要条件
   - 一旦关闭，`semanticBridgeMiss` 会明显反弹，单位 ownership 会重新恶化

### 9.10.6.2 Background validation

本轮验证继续全部走隔离桌面后台 AutoTest，没有占前台。

1. 编译：
   - `ninja -C build32` 通过
2. 在“只修 unit rigid 早退”后的后台样本：
   - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_18_26_23.html`
   - `avgFps = 0.221`
   - `avgMainThreadCpuMs = 4250.000`
   - `semanticCoreResolved = 373`
   - `semanticCoreSkinnedResolved = 373`
   - `semanticCoreSkippedNoRuntimeGroupPalette = 59`
   - `fallbackDrawCount = 656`
   - `semanticBridgeHit = 1642`
   - `semanticBridgeMiss = 16`
   - `semanticBridgeBypassed = 1555`
3. 在叠加 `VisibleRenderableRegistry` 热路径减冗余后的后台样本：
   - 报告：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_04_18_18_32_34.html`
   - `avgFps = 0.197`
   - `avgMainThreadCpuMs = 4640.625`
   - `semanticCoreResolved = 388`
   - `semanticCoreSkinnedResolved = 388`
   - `semanticCoreSkippedNoRuntimeGroupPalette = 41`
   - `fallbackDrawCount = 451`
   - `semanticBridgeHit = 1099`
   - `semanticBridgeMiss = 8`
   - `semanticBridgeBypassed = 1043`
   - `semanticSceneSubmitted = 299`
   - `semanticSceneSubmittedUnit = 261`
   - `semanticSceneSubmittedSkinned = 48`
4. 最新后台截图：
   - `AutoTest/artifacts/screenshots/war3_20260418_183235.png`
5. 当前后台抓图链的独立 blocker 仍然存在：
   - `capture_final_frame` 能成功
   - 但抓图尺寸仍是 `1902x963`
   - 还没有回到 `2560x1440` 基线

### 9.10.6.3 Real conclusion of this round

这轮已经把项目推进到了一个更清楚的状态：

1. 当前“单位阴影看起来是静态的”至少已经抓到一个明确退化点：
   - unit-like packet 曾被 `dynamicMeshRescue -> Rigid` 提前吃掉
   - 这会把本来还有机会走真正 skinned pose palette 的包，降成静态/半静态阴影
   - 这条退化点已经修掉
2. 从 summary 指标看，semantic skinned authoritative 覆盖已经被重新抬回来：
   - `semanticCoreSkinnedResolved` 从此前退化样本的 `50`
   - 恢复到 `373`，并在最新样本继续到 `388`
3. 但性能并没有同步转正：
   - 最新样本里 `avgMainThreadCpuMs` 仍在 `4.6s` 量级
   - 项目仍处于“功能继续前进，但性能不可用”的阶段
4. 当前 CPU 最大头仍然不是 shadow map 本身，也不是 `EndFrame semantic build`：
   - `Hook_WorldObjects_RenderGroup/Orig ~= 1405.612 ms`
   - `Hook_WorldObjects_RenderGroup/CollectObjects ~= 130.401 ms`
   - `SceneCollector/RegisterBatch ~= 106.717 ms`
   - `War3Renderer/EndFrame/CaptureLiveState ~= 10.388 ms`
5. 这说明本轮的真实阶段已经变成：
   - `semantic skinned resolution` 已重新恢复
   - 但 `native world render hot section + scene collector coexistence` 仍在拖垮主线程
6. 同时，这轮还不能宣布“单位阴影已经完全动态正确”：
   - 虽然 skinned authoritative 计数显著回升
   - 但后台截图仍不足以证明动作级阴影已经视觉转正
   - 下一轮仍需要做“多时刻截图差分/同单位动作变化对比”

### 9.10.6.4 Best next step after this round

基于这轮代码和后台证据，下一轮最该做的是：

1. 保持当前“unit rigid 早退已修”的状态，不要回退这条修复
2. 继续直接围绕两段热区推进：
   - `Hook_WorldObjects_RenderGroup/Orig`
   - `Hook_WorldObjects_RenderGroup/CollectObjects -> SceneCollector/RegisterBatch`
3. 优先验证并收缩：
   - `VisibleRenderableRegistry / identity bridge` 是否还有被统计到 native `Orig` 的重工作
   - `SceneCollector/RegisterBatch` 是否还能继续批量化或缓存化
4. 动态正确性上，优先补一条自动化验证能力：
   - 同单位多时刻截图差分
   - 不再只靠单帧截图肉眼判断“是不是静态阴影”

## 9.10.7 Latest round update (2026-04-18 22:15 +08:00)

### 9.10.7.1 What landed

本轮继续沿着 `explicit blend / l1cHop remap-span` 这条主 blocker 往下推，已经落地并保留的改动有：

1. `ShadowRendererCore` 的 `CollectCompactRemapSpanTables()` 改成了递归 pointer-chain 展开：
   - 不再只停在 `ownerWord -> hopWord0` 两层
   - 现在会保守展开到 4 层、最多 32 个候选
   - 会把 `hopWord0/4/8` 视作“可能的下级对象或表指针”，同时携带 `spanHint`
2. `TryBuildPackedTupleKeyWithAnySpanRemap()` / `TryBuildOrderedTupleSlotsWithAnySpanRemap()` 不再只把 `remap.table` 当最终字节表：
   - 新增 `inline window scan`
   - 会在 `remap.table + 0x01 .. +0x60` 的对象窗口内尝试寻找真正的 remap byte table
   - 这是基于 live 样本里“`remap.table` 更像对象头而不是字节表”的结论做的结构化修正
3. `SortCompactSlots()` 替代了之前的 `std::sort(begin, begin + outCount)`：
   - 清掉了这一段新加代码引出的 bounds warning
4. `runtime-group miss` 诊断增强：
   - 新增 `l1cHead` 对象窗口采样
   - 目的不是继续打印 token，而是直接观察 `l1cHop` 根对象头部 `0x00..0x1C` 的布局

### 9.10.7.2 Background validation

本轮验证继续全部走隔离桌面后台，不占前台：

1. 编译：
   - 多次 `ninja -C build32` 均通过
   - 仅保留既有 `war3_game_struct.h` reorder warning
2. 在递归 remap 候选展开落地后的后台样本（隔离桌面 + control plane 实时读取）：
   - `semanticCoreExplicitBlendAttempts = 331`
   - `semanticCoreExplicitBlendAttemptWithSpanRemapTable = 331`
   - `semanticCoreExplicitBlendStrideSearchMiss = 329`
   - `semanticCoreExplicitBlendFinalDecodeMiss = 2`
   - `semanticCoreExplicitBlendResolved = 0`
   - `semanticCoreExplicitBlendSpanRemapResolved = 0`
3. 同一轮 live 事件里，`explicit-blend miss` 新增 `tables=` 采样后得到更关键的结论：
   - `tables=69C431A0/69B92170|6AEC8B55/C33868FF|0674C985/016A018B|C320418B/CCCCCCCC`
   - `head=A0 31 C4 69 70 21 B9 69`
4. 这组数据说明：
   - 当前 `remap.table` 的头 8 字节不是 byte remap
   - 第一个候选对象头里直接出现了 pointer-like 值 `69C431A0 / 69B92170`
   - 继续追这两个值，读出来的是函数代码头/可执行段字节，不是映射表
   - 因此当前真正的 blocker 已经不是“有没有 remap 指针”，而是“`l1cHop` 根对象里的 inline table/descriptor 到底放在哪个偏移”
5. 在这之后补上 `inline window scan` 并重新部署 DLL 后：
   - 手动后台样本出现了一个新的验证异常：
     - `semanticCoreConsidered = 18`
     - `semanticCoreResolved = 0`
     - `semanticCoreSkinnedResolved = 0`
   - 这说明最新样本还不能拿来当“阴影已恢复/已转正”的签收依据
   - 但同轮 `runtime-group miss` 仍然提供了有价值的 live contract 样本：
     - `stream1=1:10000101/10101010/10101010`
     - `l1c=1:6A414BA0/00000028/00000001`
     - `poseCount=2`
     - `uniqueSlots=21`
     - `matrixIndices=23`
   - 这进一步强化了“需要用 `l1c` 根对象内部 remap，而不是继续把 `stream1` 当 direct slot”这一判断

### 9.10.7.3 Real conclusion of this round

这轮最重要的真实结论是：

1. `l1cHop` 这条线已经进一步收敛：
   - 当前 `0x6A414BA0` 这一跳不是最终 byte remap table
   - 更像“带函数指针/对象头的 descriptor 对象”
2. 所以当前主 blocker 已经被明确改写成：
   - 不是“继续往后追 pointer”
   - 而是“在 `l1c` 根对象内部找到真正的 remap byte table 或 span descriptor 偏移”
3. 这条判断比上一轮更强，因为它不是推测：
   - 是由 live `explicit-blend miss` 的 `tables=` 数据直接坐实的
4. 同时，`inline window scan` 已经进代码，但本轮还没有在 live unit path 上真正命中：
   - 当前还不能宣布单位阴影动态 contract 转正
   - 更不能说已经达到旧 VB/IB 捕获路线的画质
5. 新增的 manual sample 还暴露了一个独立风险：
   - 最新 DLL 的验证样本里，`semanticCoreResolved` 出现过直接掉到 `0` 的情况
   - 因此下一轮除了继续解 `l1c` 内联表，还必须先确认这是不是验证链抖动，还是本轮改动引入的阶段性回归

### 9.10.7.4 Best next step after this round

基于本轮代码和后台证据，下一轮最该做的是：

1. 不要再继续把 `l1cHop` 的 `word0/word4` 当最终 remap table：
   - 这条路已经被 live 样本证明会落到代码段
2. 直接围绕 `l1c` 根对象的 inline layout 做下一轮验证：
   - 利用本轮新增的 `l1cHead` 窗口日志
   - 确认真正的 remap/span 数据是否位于 `+0x10 / +0x14 / +0x18 / +0x1C` 等对象内偏移
3. 在确认 inline table 偏移前，不要再继续扩大 blind candidate 搜索范围：
   - 当前最缺的不是“更多候选”
   - 而是“更准确的对象内布局证据”
4. 下一轮还要优先确认一个回归边界：
   - 为什么最新 manual sample 会出现 `semanticCoreResolved = 0`
   - 需要先判断这是 control-plane 采样时机问题，还是本轮 remap 改动带来的真实行为回退

## 9.10.8 Latest round update (2026-04-18 23:05 +08:00)

### 9.10.8.1 What landed

本轮继续推进了两条和当前 blocker 直接相关的修正：

1. `QueryShadowRuntimeBridgeSummary()` 现在会先强制 `ShadowValidationRuntime::ensureLatestFrameBuilt()`：
   - 之前 control plane 的 `get_shadow_runtime_summary`
     只是直接读 `snapshot()`
   - 这会出现：
     - `manifest.frameNumber` 已经推进
     - 但 `semanticCoreFrameSerial` 还是旧帧
   - 这正是上一轮里 `manifest=882`、`semantic=881/0` 的可疑来源
2. `ShadowRuntimeBridgeSummary` / control plane / perf report 新增了显式“快照是否过期”字段：
   - `semanticCoreManifestFrameSerial`
   - `semanticCoreFrameLag`
   - `semanticCoreFrameFresh`
   - 后续不需要再靠人工对比 `manifest frame` 和 `semantic frame`
3. `runtime-group miss` 诊断继续保留本轮新增的 `l1cHead` 对象窗口采样：
   - 这一轮没有再回退到 blind pointer chase
   - 仍然沿着“根对象内部 inline layout”这条正确方向收口

### 9.10.8.2 Background validation

本轮验证继续全部走后台，不占前台：

1. 编译：
   - `ninja -C build32` 通过
2. 手动后台 launch/stop 链仍然不够稳定：
   - 部分样本会超时
   - 部分残留进程会进入“War3 仍存活但 control plane pipe 不再响应”的坏状态
   - 本轮已把这类残留进程都静默清掉，没有占前台
3. 这轮最重要的不是拿到新的画质签收，而是把一个关键歧义钉死：
   - 上一轮的 `semanticCoreResolved = 0`
   - 很可能至少部分来自 control plane 读取了旧 semantic snapshot
   - 而不是可以直接下结论为“semantic 主链已完全回归坏掉”
4. 当前仍未拿到一份“新字段 + 新 remap 路线 + 稳定 live sample”三者同时成立的理想样本：
   - 所以这轮不能宣布动态阴影质量转正
   - 也不能宣布 `semanticCoreResolved=0` 已被完全排除为真实回归

### 9.10.8.3 Real conclusion of this round

这轮最重要的真实结论是：

1. 上一轮里出现的 `semanticCoreResolved = 0`，现在不能再被直接当成“代码主链已坏”：
   - control plane 之前确实存在“读旧 semantic frame”的口径问题
   - 这会把验证结果污染成假回归
2. 因此当前状态要重新表述为：
   - `l1cHop` 仍然是主 blocker
   - `inline window scan` 仍未在 live unit 上命中
   - 但“semantic 直接掉到 0”这件事，已经至少部分被识别为工具链/口径歧义
3. 下一轮真正该看的，不再只是 `semanticCoreResolved` 一个数：
   - 而要同时看
     - `semanticCoreManifestFrameSerial`
     - `semanticCoreFrameSerial`
     - `semanticCoreFrameLag`
     - `semanticCoreFrameFresh`
4. 这会让后续自动化接力更可靠：
   - 不会再因为读到 stale summary 就把一个探索中的 remap patch误判成“完全打坏”

### 9.10.8.4 Best next step after this round

基于这轮代码和后台证据，下一轮最该做的是：

1. 用新加的 `semanticCoreFrameLag / semanticCoreFrameFresh` 先拿一份稳定 live 样本：
   - 先确认 summary 已经不是 stale
2. 然后继续只围绕 `l1c` 根对象内部布局推进：
   - 优先看 `l1cHead +0x10/+0x14/+0x18/+0x1C`
   - 不要再回到“继续外扩 pointer 候选”这条老路
3. 若 live 样本里 `semanticCoreFrameFresh=true` 但 explicit blend 仍是 `331/329/0` 这种模式：
   - 再继续补更细的 inline table offset 诊断
   - 目标是确认真正 remap table 是在对象头后哪个偏移开始

## 9.10.9 Latest round update (2026-04-18 23:55 +08:00)

### 9.10.9.1 What landed

本轮继续围绕 `l1cHop` / `layerStateWord1C` 的 inline descriptor 形态推进，已经落地并保留的改动有：

1. `CollectCompactRemapSpanTables()` 现在会把 descriptor 自身的 inline body 也纳入 remap 候选：
   - 不再只沿 `word0/word4/word8` 继续追 pointer
   - 新增了把 `node + 0x10`、`node + 0x18` 当作 inline table base 的候选路径
   - 若 descriptor 头里 `word4` 是可信 span，就会直接把 `+0x10/+0x18` 配成 remap candidate
2. descriptor 内二级头也开始正式入候选：
   - 若 `head10/head14` 或 `head18/head1C` 构成 `ptr + span` 组合，就会把它们加入 out-of-line remap table 候选
   - 若 `head14/head1C` 是可信 span，也会继续把 `+0x10/+0x18` 当 inline table 起点去试
3. `explicit-blend miss` 日志新增 `tags=`：
   - 现在可以直接区分命中的 remap candidate 到底来自
     - 原来的 pointer chase
     - 还是 descriptor body 的 `+0x10/+0x18`
4. `MeshLayerBindingContract` 现在新增并保留：
   - `stagePresetSpanBaseIndex`
   - `stagePresetIndex0`
   - `stagePresetIndex1`
   - 这些字段已经接入 `dynamic-mesh rescue` / `runtime-group miss` 诊断日志
   - 目的不是“先解释一切”，而是把 native `stage preset` 链正式接入当前 semantic blocker 观测面

### 9.10.9.2 Reverse-engineering convergence this round

本轮并行逆向收敛后的结论，与当前主线代码方向一致：

1. `0x6A414BA0 / word4=0x28 / word8=1` 这类头更像：
   - `DescriptorHeader { child_or_ops_ptr, span_bytes, flag } + inline body`
   - 而不是最终 byte remap table
2. 当前最值得优先尝试的内偏移已经进一步收敛成：
   - `P0: base + 0x10, span = word4`
   - `P0: base + 0x18, span = word4`
   - `P1: (+0x10, head14, head18)`
   - `P1: (+0x18, head1C, head10)`
3. 当前不应再把：
   - `word0/word4`
   - 或继续 blind pointer chase
   当成主路
4. 现有代码里已确认但此前未接入观测面的关键线索是：
   - `RenderablePart + 0x08 = stagePresetSpanBaseIndex`
   - `MeshLayerDispatchRecord + 0x0C/+0x10 = stagePresetIndex0/1`
   - 因此本轮已先把它们并到 `MeshLayerBindingContract`

### 9.10.9.3 Validation this round

本轮验证继续走后台，不占前台：

1. 编译：
   - `ninja -C build32` 通过
2. 后台一键 AutoTest：
   - DLL 部署成功
   - 进图成功
   - `runtime_status.json` 显示：
     - `runtime.runtimeReady=true`
     - `runtime.gameStarted=true`
     - `render.inGameRenderReady=true`
3. 但当前后台验证链仍有两个独立稳定性问题：
   - `capture_final_frame` 仍会超时
   - 新 perf report 仍未稳定产出
4. 另外，本轮手动 control-plane 探测脚本也出现了“进图后进程未按预期收口”的坏状态：
   - 相关残留 War3 进程已静默清理
   - 没有抢前台
5. 因此这轮**不能**把“descriptor-inline 候选已命中 live unit path”当作已签收结论：
   - 代码和方向已经推进
   - 但 live 命中还缺一份干净样本

### 9.10.9.4 Real conclusion of this round

这轮最重要的真实结论是：

1. 当前 `l1cHop` 主 blocker 已经从“继续追 pointer”正式转成：
   - “优先验证 descriptor 自身 `+0x10/+0x18` 的 inline body”
2. 本轮不是停在理论上：
   - descriptor-inline 候选已经真的进了 `CollectCompactRemapSpanTables()`
   - `stage preset` 线索也已经正式接进 layer contract
3. 但本轮还没有拿到一份足够干净的 live 样本，来证明：
   - `explicitBlendResolved` 已经从 `0` 开始抬升
   - 或者 `tags=` 里已经出现 descriptor-inline 候选命中
4. 因此当前状态应当更新为：
   - `l1c inline descriptor` 方向已继续加强
   - `stage preset` 观测面已补上
   - 但 live signing-off 仍被 control-plane/report 链稳定性拖住

### 9.10.9.5 Best next step after this round

基于本轮代码和后台证据，下一轮最该做的是：

1. 直接用当前新增的 `tags=` 观察 live `explicit-blend miss`：
   - 确认 `+0x10/+0x18` descriptor-inline 候选是否已经进入 remap table 集
2. 若仍然没有命中：
   - 继续把 `stagePresetSpanBaseIndex + stagePresetIndex0/1` 接入 remap candidate ranking
   - 不要再回到 blind pointer chase
3. 在 live 验证链上，优先拿到一份不依赖过期 perf report 的新样本：
   - 至少要确认 control-plane summary 是 fresh
   - 并能稳定读取本轮新增的 remap candidate tag 结果

### 9.10.10 RTTI export boundary update (2026-04-19 00:35 +08:00)

本轮新增利用 `docs/research/war3_RTTI/RTTI.md` 做了一次“当前命名边界”核对，结论非常明确：

1. 资源链和对象链上的核心 RTTI 真名已经进一步坐实：
   - `CSpriteUber_`
   - `CSpriteMini_`
   - `CModelComplex_`
   - `CModel`
   - `CModelData`
   - `CMaterialShared`
   - `CMaterial`
   - `CGeosetData`
   - `CGeoset`
2. 但当前 semantic-shadow 主 blocker 这一层，也就是：
   - `MeshLayerStateRecord + 0x1C`
   - `l1cHop`
   - `descriptor-inline +0x10/+0x18`
   仍然**没有**在 RTTI 导出里找到可直接对上的暴雪类名。
3. 同样地，当前代码/研究里已经使用的：
   - `RenderOverride*`
   - `StagePreset*`
   - `RenderStagePresetOverrideNode`
   - `RenderOverrideGraphOutputBundle`
   这些名字，在当前 RTTI 导出里也没有直接命中。
4. 因此必须明确区分两类结构：
   - `war3_game_structs.h` / `jass/war3_game_struct.h` 中的“稳定 ABI/offset 结构”
   - 以及“已经用 RTTI 坐实的暴雪真实类名”
5. 现阶段**不能**把 `MeshLayerStateRecord + 0x1C` 背后的对象，硬提升成某个“暴雪 RTTI 类”写进公共头；它仍应保持为：
   - 本地分析结构
   - 未命名 descriptor / record / inline body
6. 对当前开发的直接影响是：
   - 可以继续放心使用 RTTI 真名去标注资源类和对象类；
   - 但 `l1cHop` 这条线仍必须沿“descriptor-inline remap/span”去解，而不是假定已经知道真实类名。

### 9.10.10.1 Code/documentation sync this round

1. `src/d3d9/war3/core/war3_game_structs.h`
   - 顶部已补充注释，明确该头文件是“共享 ABI/offset 面”，不是“全量 RTTI 名录”。
2. 这样做的目的不是写注释本身，而是避免后续自动化线程/接力开发再次把：
   - 稳定语义结构
   - RTTI 真类名
   混为一谈。

### 9.10.11 ABI namespace cleanup + stage-preset chain extension (2026-04-19 01:10 +08:00)

本轮继续推进了两件对后续长期开发都很关键的底层清理：

1. `war3_game_structs.h` 已开始把“RTTI 真类名”和“分析结构”分层：
   - 顶层 `dxvk::war3::*` 继续只保留对象级/资源级稳定类型；
   - `SceneNode / RenderablePart / MeshLayerStateRecord / MeshLayerDispatchRecord / MeshData`
     以及 `RenderOverride* / StagePreset*` 这一批，已经降到
     `dxvk::war3::analysis::*` 命名面；
   - 目的不是改布局，而是明确“这些是稳定 ABI/analysis records，不是已经用 RTTI 坐实的暴雪类”。
2. 这轮清理已经通过：
   - `ninja -C build32`
   - 没有引入新的编译错误。

### 9.10.11.1 l1cHop diagnostics extended this round

`MeshLayerBindingContract` 这轮又往前补了一步：

1. 新增并接线：
   - `sceneStagePresetBaseIndex`
   - `resolvedStagePresetIndex0`
   - `resolvedStagePresetIndex1`
2. 当前组合逻辑是：
   - `sceneNode + 0xA0 = sceneStagePresetBaseIndex`
   - `RenderablePart + 0x08 = stagePresetSpanBaseIndex`
   - `MeshLayerDispatchRecord + 0x0C/+0x10 = stagePresetIndex0/1`
   - 然后拼成：
     - `resolvedStagePresetIndex0 = sceneBase + spanBase + idx0`
     - `resolvedStagePresetIndex1 = sceneBase + spanBase + idx1`
3. 这组组合值已经写进：
   - `runtime-group miss`
   - `dynamic-mesh rescue`
   诊断日志。

### 9.10.11.2 Why this matters

这轮的价值不是“又多了几个日志字段”，而是：

1. 现在可以直接验证：
   - `l1c` descriptor-inline 命中的 remap/span 候选
   - 与当前 draw 的 scene-level stage preset 组合索引
   是否属于同一套索引域。
2. 这比此前只看：
   - `stagePresetSpanBaseIndex`
   - `stagePresetIndex0/1`
   三个局部值更进一步，因为现在可以看到真正的“全局 preset slot”。
3. 如果下一轮 live 样本里仍然看不到关联性，那么可以更果断地判定：
   - `l1cHop` 不是 simple stage preset remap；
   - 需要继续深挖 descriptor-inline body，而不是围着 preset builder 打转。

### 9.10.12 Stage-bias decode + control-plane stabilization (2026-04-19 01:04 +08:00)

本轮继续围绕“把 `stage preset` 从纯日志字段变成真正解码输入”推进，并且同步收了一批后台自动化稳定性问题。

#### 9.10.12.1 Code changes landed this round

1. `ShadowRendererCore` 已把 `stage preset base bias` 正式接进：
   - packed tuple dynamic runtime-group 解码
   - explicit blend skinning 解码
2. 当前新增的 base-bias 候选来自：
   - `sceneStagePresetBaseIndex + stagePresetSpanBaseIndex`
   - `resolvedStagePresetIndex0 - stagePresetIndex0`
   - `resolvedStagePresetIndex1 - stagePresetIndex1`
3. 这意味着：
   - `raw slot`
   - 以及 `remap.table[raw]`
   不再只会被默认解释成 `pose[0..n]`；
   - 当 layer contract 给出了可信的 stage-preset 基址时，
     现在会尝试把 tuple slot 映射到 `baseBias + localSlot`。
4. 同一轮里，`CollectCompactRemapSpanTables()` 产出的 remap candidates
   已经在 decode 侧按 provenance 做优先级排序：
   - 优先 `ownerWord == layerStateWord1C`
   - 其次 `ownerWord == layerStateWord20`
   - 同时优先 descriptor-inline `+0x10/+0x18/+0x114/+0x118/+0x11C` 一带
     产出的 tag
5. 显式 blend 成功日志也新增了：
   - `stageBias`
   这样后面 live 样本一旦命中，就能直接确认到底是不是 `stage-preset base bias`
   打开的。

#### 9.10.12.2 AutoTest / control-plane fixes landed this round

1. `d3d9_swapchain.cpp`
   - `ProcessPendingFrameCapture(...)` 已经从 `HookUi` 分支里移出；
   - 现在只要 `Diag` 模块开启，就会在 present 链上尝试处理 pending capture；
   - 不再要求这一帧一定走到 UI hook 才能消费截图请求。
2. `AutoTest/war3_autotest_mcp.py`
   - 当本轮没有生成新的 perf report 时，
     不再把旧 `latest report` 直接伪装成成功结果返回；
   - 现在会显式标出：
     - `latestReportPath`
     - `newReportDetected=false`
     - `reportWasStale=true`
   - 这样自动化线程不会再把过期报告当成本轮真实签收。

#### 9.10.12.3 Validation completed this round

1. `ninja -C build32` 已通过。
2. `python -m py_compile AutoTest/war3_autotest_mcp.py` 已通过。
3. 后台隔离桌面 `run_named_scenario('dynamic_shadow_pressure')` 已再次跑通到：
   - deploy
   - ready
   - stop
   但截图和新报告仍未稳定产出；
   当前自动化现在会诚实地返回：
   - `reportWasStale=true`
4. 一轮更短的后台 control-plane 采样已经确认：
   - `get_shadow_runtime_summary`
   - `get_frame_manifest_summary`
   在进图后可直接返回
   - 不依赖截图链
5. 但这份短采样仍是“刚进图早帧”：
   - `semanticCoreResolved = 0`
   - `semanticCoreFrameFresh = false`
   - `visibleCount = 47`
   - `unitCount = 0`
   - `recordsWithStableIdentity = 29`
   - `recordsWithResolvedGeoset = 13`
   - `shadowRuntimeModelCount = 182`
   - `shadowModelResourceCount = 21`
6. 因此这份样本**不能**拿来判断：
   - `stageBias` 是否已经让 `explicitBlendResolved` 真正抬起来
   - 也不能拿来否定新 decode 路径
7. 当前自动化链的新明确结论是：
   - 早帧 control-plane summary 现在可直接拉取；
   - 但“热帧 summary”仍需单独取样，不能把 `wait_for_game_ready()` 命中的首帧摘要当最终判断。

#### 9.10.12.4 What is true after this round

1. `stage preset` 现在已经不只是日志字段，而是 semantic skinned decode 的真实输入之一。
2. `l1cHop` 这条线的下一步，已经从“继续加字段”收敛成：
   - 拿到一份热帧 live summary / miss sample
   - 看 `stageBias` 与 provenance 排序之后，
     `explicitBlendResolved` 是否开始从 `0` 脱离
3. 控制面/自动化层面现在也更诚实了：
   - 截图链仍不稳定
   - 新报告仍不稳定
   - 但不会再把 stale report 假装成成功。

#### 9.10.12.5 Best next step after this round

1. 优先做“ready 后热一段时间”的 control-plane 摘要采样：
   - 不依赖 screenshot
   - 不依赖 perf report
   - 直接看热帧：
     - `semanticCoreExplicitBlendAttempts`
     - `semanticCoreExplicitBlendResolved`
     - `semanticCoreExplicitBlendStrideSearchMiss`
     - `semanticCoreSkinnedResolved`
2. 如果 `explicitBlendResolved` 仍然是 `0`：
   - 下一刀优先把 remap provenance 从 decode helper 继续带到 miss 统计里；
   - 明确看到“到底是哪一个 owner/tag 被尝试并失败”。
3. 如果 `explicitBlendResolved` 开始抬升：
   - 立刻转向收视觉正确性和性能；
   - 继续削减 legacy object fallback。

### 9.10.13 Owned-handle decode correction + live rootOwned probe (2026-04-19 03:40 +08:00)

本轮把 `runtimeModel + 0x9C` 这条线从“默认按 direct model resource 猜测”正式收窄成了
“只接受已经直接长成 `CModelData` header 的极窄命中”，并且补了一轮新的 live probe。

#### 9.10.13.1 Code changes landed this round

1. `war3_model_hook.cpp`
   - `TryResolveOwnedModelResourcePtr(...)` 已改名为
     `TryResolveDirectModelResourceFromRuntimeModel(...)`
   - 现在只会：
     - 读 `runtimeModel + 0x9C`
     - 再用 `ShadowModelResourceCache::isDirectModelResourcePtr(...)`
       做直接命中校验
   - 不再把 `+0x9C` 当成“可继续深扫 / 自动 unwrap 出 resource”的默认来源
2. `war3_visible_renderables.cpp`
   - `TryReadOwnedModelResourceFromRuntimeModel(...)` 同样改成
     `TryReadDirectModelResourceFromRuntimeModel(...)`
   - `VisibleRenderable` 热路径不再因为 `runtimeModel + 0x9C` 去做深层 model-resource
     猜测，只在它本身已经长得像 direct `CModelData` 时才接受
3. `war3_shadow_renderer_core.cpp`
   - `runtime-group miss` 日志已把原来的 `rootOwned` 明确重命名成
     `rootOwnedHandle`
   - 并新增输出：
     - `rootHandleFields[flags=%08X proto=%p self=%p]`
   - 方便直接验证这个对象自身的 `+0x94/+0x98/+0x9C` 是不是像
     `CModelData` 一样可解释

#### 9.10.13.2 New reverse / runtime conclusion from this round

1. IDA 新确认的关键事实：
   - `0x6F12EC90`（`CModelComplex__RecurseChildRuntimeTree`）会直接把
     `runtimeModel + 0x9C` 当成第二个参数一路往下传
   - 并且第一步就把这个参数做：
     - `sub_6F77C280(controller, arg2 + 0x5C)`
2. 这说明：
   - `runtimeModel + 0x9C` 在 runtime pose / child tree 传播里承担的语义，
     并不等同于“给我们读 geoset header 的 direct `CModelData` resource root”
   - 即使它在某些文档里被记成 retained `HMODELDATA`，当前 live draw path
     上也不能再把它直接当成 authoritative model-resource base
3. 结合 live probe，本轮更可信的工程结论是：
   - `runtimeModel + 0x9C` 目前应该被视为 **owned handle / runtime helper object**
   - 它对 pose / controller 传播有用
   - 但对上层阴影几何恢复来说，当前不能继续作为“默认的 direct resource 入口”
   - 真正稳定的几何恢复仍应优先走：
     - `runtimeModel + geosetIndex`
     - `runtimeGeosetPtr`
     - `runtimeGeosetDataPtr`

#### 9.10.13.3 Background validation completed this round

1. `ninja -C build32` 已通过（两轮）。
2. `python -m py_compile AutoTest/war3_autotest_mcp.py` 已通过。
3. 后台隔离桌面短样本（`low_pressure_static_reuse`）已再次完成：
   - deploy
   - ready
   - silent stop（`avoid_foreground_switch=True`）
4. 当前热样本的新硬证据：
   - `DXVK SemanticCore: runtime-group miss ...`
   - `rootOwnedHandle=1ac72844`
   - `rootHandleFields[flags=4331B8D5 proto=434e2e14 self=c2b9672b]`
   - `rootHead=1:6A41172C/0000000D/C3642CCD/C366B4FE|C27D1FA4/4361C5A2/435F3D71/437E4042|BF99CAC0/C06EF1A0/42BEF859/00000000`
5. 这份 live 样本的直接含义是：
   - 这个 `rootOwnedHandle` 至少在当前 sampled unit path 上，
     它自己的 `+0x94/+0x98/+0x9C` 已经明显不是正常的
     `CModelData { flags / shared_resource_proto / model_data_handle }`
     语义
   - 因此继续把 `runtimeModel + 0x9C` 当成热路径 direct model-resource 去解，
     当前已经没有依据

#### 9.10.13.4 Current blocker after this round

1. `semanticCoreResolved=0 / unitCount=0` 这类“早帧空样本”仍然会在 control-plane
   里出现，当前不能直接拿首帧 summary 判断新 decode 路径是否失败
2. `named pipe 121` / `get_shadow_runtime_summary timeout` 仍未根治
3. 但这轮至少已经把一个会持续误导工程判断的假设打掉了：
   - `runtimeModel + 0x9C` 不是当前应该继续深挖的 direct resource root

#### 9.10.13.5 Best next step after this round

1. 下一刀优先继续收 `runtimeModel runtimeGeosets / runtimeGeosetData`
   这条 authoritative resource path，不再围着 `+0x9C` 做热路径 model-resource
   猜测
2. 同时单独逆向：
   - `0x6F12EC90 -> sub_6F77C280(arg2 + 0x5C)`
   看 `rootOwnedHandle` 在 runtime pose / visible-part 控制里到底是什么对象
3. control-plane 侧则单独收：
   - 单线程 pipe 服务被长命令占住
   - `get_hot_shadow_probe` 三段快照跨代读取
   这两个稳定性问题

### 9.10.14 Control-plane parallel client handling (2026-04-19 03:50 +08:00)

本轮顺手把 control-plane 的一个明确稳定性痛点也收了一刀：服务端不再串行卡住所有 pipe 请求。

#### 9.10.14.1 Code changes landed this round

1. `war3_control_plane.cpp`
   - 原来的 `ServerLoop()` 是：
     - `CreateNamedPipe`
     - `ConnectNamedPipe`
     - 同线程 `ReadAll -> HandleCommand -> WriteAll`
     - 处理完当前请求以后，才会继续 accept 下一个连接
   - 现在已经改成：
     - `ServerLoop()` 只负责 accept
     - 每个连接交给 `HandlePipeClient(HANDLE pipe)` 独立 worker thread
     - listener 会立刻回到下一轮 `CreateNamedPipe / ConnectNamedPipe`
2. 新增：
   - `g_activeClientCount`
   - `HandlePipeClient(...)`
3. 这意味着：
   - `wait_until`
   - `capture_final_frame`
   - `invoke_test_command`
   这类长请求不再天然阻塞所有其它
   `get_shadow_runtime_summary / get_runtime_status / get_hot_shadow_probe`
   请求

#### 9.10.14.2 Validation completed this round

1. `ninja -C build32` 已通过。
2. 后台隔离桌面并发验证已完成：
   - 一个线程发起长 `wait_until(timeoutSec=6, minFrameIndex=99999999)`
   - 另一条并发请求同时拉 `get_shadow_runtime_summary`
3. 当前结果：
   - `get_shadow_runtime_summary` 已可在长 `wait_until` 持续期间直接返回成功
   - 说明 control-plane 不再被单线程串行 accept/handle 彻底堵死
4. 注意：
   - 这轮并不代表所有 `pipe 121 / timeout` 都已经消失
   - 但“单线程 server 被长请求整条卡住”这个已知瓶颈已经被削掉

#### 9.10.14.3 Current meaning after this round

1. 之后夜间 AutoTest / control-plane 热帧采样会更可靠：
   - 即使有一个长轮询请求在跑
   - 旁路 summary / probe 也更容易拿到返回
2. 当前 control-plane 还剩下的更真实问题收敛成：
   - `get_hot_shadow_probe` 三段快照跨代
   - 以及个别请求自身的超时/首帧空样本
3. 也就是说：
   - 这轮修的是“服务端并发结构”
   - 不是“热帧语义摘要已经完全正确”

### 9.10.15 Footman MDL ground-truth correlation + pose-selection correction (2026-04-19 11:16 +08:00)

本轮开始正式利用用户提供的 `C:\\Users\\Administrator\\Desktop\\Footman.txt`
作为 live runtime 对照尺，而不是继续只靠抽象 runtime miss 日志。

#### 9.10.15.1 Ground-truth extracted from Footman.txt

1. `Footman` 明确是 classic MDL 路线：
   - `NumGeosets = 5`
   - `NumBones = 25`
2. 关键 geoset 签名：
   - `geoset[0]`
     - `vertexCount = 447`
     - `groupCount = 35`
     - `matrixIndices = 54`
     - 第一批 `VertexGroup = [11, 12, 11, 34, 34, ...]`
   - `geoset[1]`
     - `vertexCount = 52`
     - `groupCount = 9`
   - `geoset[2]`
     - `vertexCount = 26`
     - `groupCount = 4`
   - `geoset[3]`
     - `vertexCount = 6`
     - `groupCount = 1`
   - `geoset[4]`
     - `vertexCount = 51`
     - `groupCount = 1`

#### 9.10.15.2 New hard correlation proven this round

1. 旧自动化样本里有一条与 Footman 主 body geoset 完全匹配的
   `runtime-group miss`：
   - `geoIdx=0`
   - `vertexGroups=447`
   - `uniqueSlots=35`
   - `groupCount=35`
   - `matrixIndices=54`
   - `sample1=0x220B0C0B`
2. 结合 `Footman.txt`，现在已经能直接证明：
   - `0x220B0C0B` 的 little-endian 4 字节就是：
     - `[0x0B, 0x0C, 0x0B, 0x22]`
     - 也就是 `VertexGroup[0..3] = [11, 12, 11, 34]`
3. 这条证据的意义非常重要：
   - 至少对 `Footman` 这类 classic skinned unit 主 geoset 来说，
     `meshStream1` 已经不是“噪声或未知 packed tuple”
   - 它已经直接承载了顶点 group slot 数据
4. 因此当前主 blocker 进一步收敛为：
   - 不是“我们还没拿到顶点 group slot”
   - 而是“这些 slot 在当前 draw 上到底吃哪份 pose/runtime group palette”

#### 9.10.15.3 Code changes landed this round

1. `war3_shadow_renderer_core.cpp`
   - 新增：
     - `GetUsablePoseMatrixCount(...)`
     - `ShouldPreferPoseCandidate(...)`
     - `TryResolveBestPoseForRenderable(...)`
2. 现在 `resolveRecord(...)` 的 pose 选择不再是：
   - `runtimeModel` 命中就直接用
   - 只有完全 miss 才回退 `sceneNode`
3. 新逻辑改成：
   - 在 `runtimeModel`
   - `sceneNode`
   - `meshPoseCtx`
   三类候选里，挑出“真正可用且矩阵更多”的 pose
4. 这轮修正的工程动机是：
   - `Footman` 主 geoset 明明需要 `maxSlot=34` 级别的 group palette
   - 但旧 `runtime-group miss` 样本里却出现过 `poseCount=2`
   - 这更像是 body geoset 被错误绑到了 child runtime / part runtime 的小 palette
   - 而不是 `VertexGroup` 本身还没解开

#### 9.10.15.4 Validation boundary this round

1. `ninja -C build32` 已通过。
2. `Footman.txt` 与旧 runtime miss 的结构对照已经坐实。
3. 但后台 live 验证目前仍未完全签收：
   - 新一轮 isolated-desktop 样本能稳定进到 `debug-events ready`
   - 但 `capture_final_frame` 仍会 timeout
   - `stop_war3(... silent stop ...)` 仍有残留进程问题
   - control-plane 热帧摘要仍可能 timeout 或停在首帧空状态
4. 当前 live 样本已经能看到：
   - `DXVK SemanticCore: build manifest=... resolved=0 skinned=0 skipNoId=18`
   - 说明这轮更早暴露出来的瓶颈变成：
     - semantic visible identity / runtime warmup 还没进入真正的 hot frame
     - 还不能只靠这一轮首帧 build 把新 pose 逻辑判定为失败

#### 9.10.15.5 Best next step after this round

1. 继续沿 `Footman` 这条 concrete sample 收：
   - 既然 `stream1 == VertexGroup` 已坐实
   - 下一步应优先验证 body geoset 使用的 pose 是否仍被 child runtime 小 palette 覆盖
2. 验证链侧优先补：
   - `capture_final_frame` timeout
   - `silent stop` 残留进程
   - hot-frame summary 首帧空样本
3. 只有拿到一份真正的 hot live sample，才能确认这轮
   “best pose candidate” 修正是否已经把 `Footman` 从
   `poseCount=1~2` 的错误姿态里拉出来

### 9.10.16 SemanticCore skip-path split + isolated-desktop stall guard (2026-04-19 11:43 +08:00)

本轮把最近最容易误导判断的两个问题拆开并各自落地了：

1. `SemanticCore` 的 `resolved=0` 现在不再只有一个 `skipNoId` 指标，
   而是正式拆成：
   - `skipNoId`
   - `skipNoGeo`
   - `skipNoPose`
   - `skipNoGrp`
2. `AutoTest` 的 `wait_for_game_ready(allow_fallback=False)` 现在会在
   `debug-events ready` 之后额外做一次短窗口 frame-progress 复核。
   如果只是“首帧 ready 但 `frameIndex` / `dispatchCalls` 不再推进”，
   就直接返回 `debug-events-stalled`，不再把它当成有效热帧。

#### 9.10.16.1 Code changes landed this round

1. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - `registerMainQueueRange(...)` 新增从 `RenderQueueTracker` 回收
     per-element cached identity 的 fallback。
   - 目的：
     - 避免 `RenderBatch_Submit` 当下的 sceneNode identity 解析失手时，
       明明已经在 `RenderQueue_AddBatch` prime 过的对象，又整批掉成空身份。
2. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `DXVK SemanticCore: build ...` 日志现在增加：
     - `skipNoGeo`
     - `skipNoPose`
     - `skipNoGrp`
   - 同时新增 `DXVK SemanticCore: skip ...` 诊断：
     - `no-identity`
     - `no-resolved-geoset`
     - `resource-miss`
     - `resource-not-ready`
     - `no-pose`
3. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - 尝试把 `meshData + 0xF0` (`TransformOrPoseCtx`) 接回
     `ResolveModelMetadata(...)` 作为 runtime-model hint。
   - 但在 live cold-frame 样本里读出了 `0x00000001` 这种假值；
     因此本轮已经把这条入口收紧为“必须先长得像可读 `CModel` runtime object”
     才接受，避免污染主链。
4. `AutoTest/war3_autotest_mcp.py`
   - 新增 `_read_runtime_status_best_effort(...)`
   - `wait_for_game_ready(...)` 在 `allow_fallback=False` 时，
     若 `debug-events ready` 后：
     - `frameIndex` 不推进
     - `dispatchCalls` 不推进
     - `render.inGameRenderReady=true`
     - 但 `render.isInGame=false`
     就返回：
     - `mode = debug-events-stalled`
     - `error = debug-events ready 后 frameIndex 未继续推进，疑似首帧卡住`

#### 9.10.16.2 New runtime conclusions from this round

1. 当前 `resolved=0` 的 live 失败，不只是 `skipNoId=18`。
   通过新日志已经明确看到更大的失败量来自：
   - `skipNoGeo = 29`
2. 这些 `resource-miss` 记录已经具备：
   - `sceneNode != nullptr`
   - `meshData != nullptr`
   - `meshIndex/geosetIndex = 0..3`
   - 但：
     - `runtimeModelPtr = nullptr`
     - `modelResourcePtr = nullptr`
     - `runtimeGeosetPtr = nullptr`
     - `runtimeGeosetDataPtr = nullptr`
3. 典型 live 样本（本轮新增 `skip resource-miss` 日志）：
   - `scene=1b750624 mesh=11805b14 geoIdx=0 q=0`
   - `scene=1b751664 mesh=118060dc geoIdx=0 q=1`
   - 说明当前冷帧的主要断点已经不是“看不到 geoset index”，
     而是“已有 sceneNode + meshData + geoIdx，但 runtime model/resource
     还没绑定回来”。
4. `meshData + 0xF0` 在这些冷帧样本里不能盲信成 runtimeModel：
   - 曾直接读出 `0x00000001`
   - 所以这条入口必须经过“像不像 `CModel` runtime object”校验，
     不能再裸用。

#### 9.10.16.3 Validation completed this round

1. `ninja -C build32` 多轮通过。
2. `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
3. 后台隔离桌面样本新增一个极重要的稳定性结论：
   - `runtime_status` 会持续刷新时间戳
   - 但：
     - `frameIndex = 882`
     - `dispatchCalls = 1771`
     - `render.isInGame = false`
   - 在多秒窗口内保持不变
4. 也就是说：
   - 当前隔离桌面链路可以打到：
     - `gameStarted=true`
     - `jassReady=true`
     - `runtimeReady=true`
     - `inGameRenderReady=true`
   - 但**并不等于真正进入持续推进的 in-game hot frame**
5. 新的 `wait_for_game_ready(allow_fallback=False)` 已实测会把这种情况
   直接判成：
   - `debug-events-stalled`
   而不再误报成功。

#### 9.10.16.4 Current meaning after this round

1. 现在已经可以明确区分两个问题：
   - **主线语义/资源问题**：
     - 冷帧里存在 `resource-miss`，sceneNode/meshData 已在，
       但 runtime model/resource 还没回来
   - **验证宿主问题**：
     - 隔离桌面后台链当前会停在首帧附近，
       不能作为“热帧 dynamic shadow 已签收”的依据
2. 因此：
   - `skipNoId=18` 不再是唯一主 blocker
   - `skipNoGeo=29` 的 `resource-miss` 现在已经成为更真实的主断点
3. 同时，之后任何后台 AutoTest 结果若停在：
   - `debug-events-stalled`
   都应该首先视为“验证宿主 stalled”，而不是立刻推导
   `SemanticCore` 主逻辑已失败。

#### 9.10.16.5 Best next step after this round

1. 主线继续优先收：
   - `sceneNode + meshData + geoIdx` 已在时，
     runtime model/resource 还能从哪里 authoritative 恢复
2. 逆向侧继续沿：
   - `l1cHop / descriptor-inline remap-span`
   收 dynamic skinned contract
3. 验证链侧则必须承认当前现实：
   - 隔离桌面后台链现在是“首帧/冷帧观测工具”
   - 不是“hot in-game dynamic shadow 最终签收工具”
   - 真正热帧签收需要先解决 background render stall，
     或改用不会冻结 frameIndex 的宿主路径

### 9.10.17 Visible manifest runtime-model recovery via RenderObject + runtime bridge (2026-04-19 12:02 +08:00)

本轮继续沿 `resource-miss` 主线推进，但不再扩散到新的 blind memory probe。
核心思路是：既然当前 `skipNoGeo` 的记录已经具备
- `sceneNode`
- `meshData`
- `geosetIndex`

那么首先应该把 **已有 registry/runtime bridge 里已经知道的
`runtimeModel/modelResource` 正式回灌到 visible manifest**，
而不是继续让 `SemanticCore` 在消费阶段承担这部分恢复成本。

#### 9.10.17.1 Code changes landed this round

1. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - `ResolveModelMetadata(...)` 新增了两条更权威的恢复路径：
     1. **`RenderObjectRegistry` 场景恢复链**
        - 按 `sceneNode -> worldObjectEntry -> jHandle` 查询 `RenderObjectInfo`
        - 若拿到 `unitPtr`，则走：
          - `unitPtr -> sprite`
          - `sprite -> runtimeModel`
          - `ModelRegistry.findBySprite/findByRuntimeModel`
        - 目的：修复“冷帧里 `sceneNode` 已知，但 `identity.unitPtr`
          尚未挂回 visible record”时的 model metadata 缺口。
     2. **runtime bridge augment 兜底**
        - 当常规 registry 路径仍未补回
          `runtimeModel/modelResource/modelKey` 时，
          构造一个最小 `War3ShadowSemanticContext`
          并调用 `AugmentShadowSemanticContext(...)`
        - 目的：直接复用 runtime bridge 已经实现好的
          `sceneNode/jHandle/worldObjectEntry -> runtimeModel/modelResource`
          修补逻辑，而不是让 visible manifest 再维护一套更弱的平行副本。
2. 本轮没有重新放宽 `meshData + 0xF0` 的直读路径：
   - `TryReadRuntimeModelFromMeshData(...)` 仍保留“必须像可读 `CModel`
     runtime object”校验；
   - 防止再次把 `0x00000001` 一类伪值污染 runtimeModel 主链。

#### 9.10.17.2 Why this round matters

1. 上一轮已经明确：
   - 当前 live 冷帧失败中，
     真正更大的 bucket 是 `skipNoGeo=29`
   - 不是单纯 `skipNoId`
2. 这些记录已经具备：
   - `sceneNode != nullptr`
   - `meshData != nullptr`
   - `meshIndex/geosetIndex` 合法
3. 所以本轮的正确推进方向不是继续扫 `meshData` 内部字段，
   而是优先把：
   - `sceneNode -> RenderObjectInfo`
   - `sceneNode/jHandle/worldObjectEntry -> runtime bridge`
   这两条已有上层恢复链并回 visible manifest。

#### 9.10.17.3 Validation completed this round

1. `ninja -C build32` 通过。
2. 后台隔离桌面 AutoTest 复测通过：
   - 可 deploy 新 DLL
   - 可进图 / ready
   - 可 silent stop 且不抢前台
3. 当前验证宿主仍存在旧边界：
   - `get_runtime_status` 能正常返回
   - 但：
     - `get_shadow_runtime_summary`
     - `get_frame_manifest_summary`
     在当前隔离桌面样本里仍会 timeout
4. 因而这轮还没有拿到一份可靠的 hot-frame semantic summary，
   不能把“`skipNoGeo` 已显著下降”冒充成已签收结论。

#### 9.10.17.4 Current meaning after this round

1. `resource-miss` 的修复方向已经进一步收敛：
   - **优先用 sceneNode/object/runtime bridge 的现成绑定修复**
   - 而不是回到 `meshData` blind pointer guess
2. 这也意味着：
   - 接下来若 `skipNoGeo` 仍不下降，
   - 更值得怀疑的是“sceneNode 当前帧没有 authoritative 绑定回来”，
     而不是 `stream1/group/palette` 主线又退化了
3. 当前最大未签收点仍是验证链：
   - 隔离桌面可 ready
   - 但 semantic summary 指令仍可能 timeout
   - 所以 runtime bridge / visible manifest 的这轮补强，
     还需要下一轮继续拿到一份更可靠的 live semantic summary 来签收

### 9.10.18 IDA confirmed the old l1cHop-first blocker framing was wrong; pivot to group-blended palette (2026-04-19 12:44 +08:00)

本轮不是再补更多 `l1cHop` 日志，而是直接回到 IDA 把
`skinned path` 的真正 CPU 合同重新核了一遍。结论已经足够明确：

1. **我们此前把 `l1cHop / remap-span` 当成 primary blocker 的路线是错的。**
2. War3 的 authoritative skinned path 不是在 draw 前临时解一张
   “vertex -> hidden remap byte table”，而是：
   - 先解完整体 pose
   - 再按 `CGeosetData::MatrixGroupSizes/MatrixIndices`
     在 CPU 端构造一张 **per-draw group-blended palette**
   - 最后让 `VertexGroup` 直接索引这张 palette

#### 9.10.18.1 IDA evidence confirmed this round

1. `0x6F12E900 = CModel_PrepareRenderPalettesAndDispatch`
   - 已确认会先求 pose，再调用后续 palette 分配/填充链。
2. `0x6F12FED0 = CModel_AllocAndFillGroupPalette`
   - 已确认会：
     - 为当前 batch item 分配 slot
     - 取回 palette slot 指针
     - 调 `CGeosetData_BuildGroupBlendedPalette(...)`
3. `0x6F12E600 = CGeosetData_BuildGroupBlendedPalette`
   - 已确认按 `CGeosetData +0xF0/+0xF4/+0x100`
     解释 `MatrixGroupCount / MatrixGroupSizes / MatrixIndices`
   - 然后逐 group 调 `CMatrixGroup_BlendOutputMatrix(...)`
4. `0x6F12E200 = CMatrixGroup_BlendOutputMatrix`
   - 已确认：
     - 单骨骼：直接拷贝
     - 双骨骼：乘 `0.5`
     - 三骨骼：乘 `0.33333331`
     - N 骨骼：累加后乘 `1/N`
   - 也就是说，**当前 authoritative 合同是 group 内均值混合，不是自定义 float weight 表。**
5. `0x6F13A510 = RenderQueue_UpdateItemWorldMatrix`
   - 已确认会用 `item + 8` 取 palette slot
   - 通过 `RenderQueue_GetPaletteSlotPtr(...)`
     直接拿到 48-byte * N 的矩阵块供 dispatch 侧使用

#### 9.10.18.2 Meaning after this reverse confirmation

1. `Footman` 上已经坐实：
   - `stream1 == VertexGroup`
2. 再结合本轮 IDA 结果，现在最可信的主合同已经变成：
   - `static positions/indices`
   - `resource.vertexGroupIndices`
   - `resource.matrixGroupSizes`
   - `resource.matrixIndices`
   - `pose.matrixPalette`
   - `TryBuildRuntimeGroupPalette(...)`
3. 因此：
   - `l1cHop / descriptor-inline remap-span`
   - `explicit blend`
   - `dynamic group override`
   不该再作为 **primary path**；
   它们最多只应保留为 fallback / diagnostic。

#### 9.10.18.3 Code pivot landed this round

1. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `resolveRecord(...)` 已开始正式 pivot：
     - 不再让 `tryDynamicMeshRescue(...)` 在 canonical group-palette 之前抢跑；
     - `TryBuildRuntimeGroupPalette(...)` 失败后，才允许：
       - mesh-pose / child-runtime pose rescue
       - dynamic-mesh rescue
       - explicit-blend rescue（最后兜底）
2. 同时：
   - 当 `TryBuildRuntimeGroupPalette(...)` 已成功时，
     不再继续让 `explicit-blend skin` / `dynamic-group override`
     覆盖 canonical packet。
3. 这意味着从本轮开始：
   - `CGeosetData group palette + VertexGroup`
     已经重新成为 authoritative skinned path；
   - 旧 `l1cHop` 线正式降级为 fallback 验证分支。

#### 9.10.18.4 Current blocker after the pivot

1. 现在真正剩下的 blocker 已经更清楚了：
   - **不是**“还没解开 remap-span byte table”
   - 而是：
     - runtime model/resource authoritative 恢复
     - pose 来源是否选对 runtime instance
     - 热帧验证链是否稳定拿到 semantic summary
2. 换句话说：
   - 路线已经转正
   - 但是否彻底修复“单位阴影静态/块状/消失”，
     还需要下一轮 build + runtime sample 来签收

#### 9.10.18.5 Validation completed immediately after the pivot

1. `ninja -C build32` 已通过。
2. 后台隔离桌面重新跑了 `dynamic_shadow_pressure` / 自定义 control-plane probe：
   - 新 DLL 可 deploy
   - 可 ready
   - 可 silent stop
   - 不抢前台
3. 但当前隔离桌面宿主边界仍然没变：
   - `get_runtime_status` 可稳定返回
   - 且持续显示：
     - `frameIndex = 882`
     - `dispatchCalls = 1771`
     - `render.inGameRenderReady = true`
     - `render.isInGame = false`
   - 同时：
     - `get_shadow_runtime_summary`
     - `get_frame_manifest_summary`
     仍会超时
4. 因而本轮已经可以签收的只有两件事：
   - **架构判断已确认**：旧 `l1cHop-first` 路线错误，主线已 pivot
   - **代码结构已确认**：canonical skinned path 已回到
     `TryBuildRuntimeGroupPalette(...)`
5. 但还不能把本轮说成“视觉正确性已 runtime 签收”，因为当前后台宿主
   仍然停在首帧/冷帧边界，拿不到真正的 hot-frame semantic summary。

### 9.10.19 Direct `CGeosetData*` batch-item path is now wired in, and control-plane summary no longer blocks (2026-04-19 13:03 +08:00)

本轮不是继续追 `modelResource == 0` 本身，而是沿着新补强后的逆向事实继续收：

1. 你补进 [SKINNING_PIPELINE_REVERSED_2026_04_19.md](./SKINNING_PIPELINE_REVERSED_2026_04_19.md)
   的主链明确指出：
   - `batch_item + 0x0C == CGeosetData*`
   - 而不是一律 `MeshData*`
2. 这一点和我们先前在匿名 `resource-miss` 样本里看到的
   `mesh + 0x108 -> 0..3`
   完全对上：
   - 若把该指针当 `MeshData*`，`+0x108` 会被误读成 geoset index
   - 但若把它当 `CGeosetData*`，`+0x108` 更像
     `LayoutOrMaterialSlot`

#### 9.10.19.1 Code changes landed this round

1. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - `ResolveGeosetMetadata(...)` 先尝试把 `renderablePart/payload + 0x0C`
     识别成 `CGeosetData*`：
     - 若 `ShadowModelResourceCache` 已能按 data 命中，直接回填
       `runtimeGeosetDataPtr`
     - 否则按 `CGeosetData` 形状做轻量识别
   - 只有在**不像 geosetData**时，才继续把它当 canonical `MeshData*`
     去读 `MeshIndex`
2. `src/d3d9/war3/model/war3_model_resource_cache.cpp`
   - `noteRuntimeGeosetBinding(...)` 不再因为
     `geosetIndex == invalid`
     就直接早退；
     现在允许“只有 `runtimeGeosetDataPtr`、没有 geoset index”的
     data-only 记录先进入 cache。
   - `snapshotGeosets()` / `geosetRecordCount()` / `readyGeosetCount()`
     不再只看 `m_byGeoset`；
     现在会把 `m_byGeosetData` 中那些
     `geosetPtr == nullptr` 的 data-only 记录一并导出/计数。
3. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `get_shadow_runtime_summary`
     不再强制同步 refresh semantic frame；
     改为返回最新已发布快照，并依赖
     `semanticCoreFrameFresh / semanticCoreFrameLag / semanticCorePublishRevisionLag`
     来判断是否陈旧。
   - 这样 control-plane 不会再因为“为了追热帧再 build 一次”
     而把 pipe 阻塞到超时。

#### 9.10.19.2 Runtime validation completed this round

1. `ninja -C build32` 已通过（包含上述 3 处改动）。
2. 后台隔离桌面 direct probe 已重新验证：
   - `get_frame_manifest_summary` 继续稳定返回
   - `get_shadow_runtime_summary` 现在也能在 control-plane 上
     **快速返回**
3. 最新样本（`War3SummaryFastProbe`）中，关键结果是：
   - `semanticCoreSkippedNoGeoset = 16`
   - `semanticCoreSkippedNoIdentity = 18`
   - `semanticCoreSkippedNoPose = 13`
   - `semanticCoreResolved = 0`
   - `semanticCoreFrameFresh = false`
   - `semanticCoreFrameLag = 1`
   - `shadowReadyGeosetCount = 1944`
   - `shadowModelResourceCount = 0`
4. 对比上一轮旧样本：
   - 旧 `skipNoGeo` 口径曾落在 `29`
   - 本轮已下降到 `16`
   - 说明：
     **direct geosetData batch-item path 已经真实减少了一部分
     `resource-miss / no-geoset`**

#### 9.10.19.3 What changed in the blocker picture

1. 当前主 blocker 不再是：
   - `l1cHop`
   - 或“所有 geoset 都还没恢复回来”
2. 当前剩余问题已经进一步收缩成：
   - 一部分匿名/弱身份 record 仍是 `no-identity`
   - 一部分 record 已有 geoset，但仍然 `no-pose`
   - semantic snapshot 仍然是
     `frameFresh=false, frameLag=1`
     的“冷一帧”状态
3. 同时要明确：
   - `recordsWithModelResource = 0`
   - `shadowModelResourceCount = 0`
   这件事仍然存在；
   但它已经不再是“继续推进前的硬门槛”，因为 canonical resource 路线
   已经开始直接吃 `runtimeGeosetDataPtr`

#### 9.10.19.4 Next step

下一刀不再追 `modelResource == 0` 本身，而是：

1. 收 `no-pose = 13`
   - 确认这些已经拿到 geoset 的 record，为什么最终没有命中 pose
2. 收 `no-identity = 18`
   - 尤其是匿名 `sceneNode` / `queueKind=transparent` 的剩余路径
3. 继续利用现在已经不超时的 `get_shadow_runtime_summary`
   直接签收：
   - canonical group-palette 是否在 hot-frame 上开始真正产出 draw packet
   - 而不是再依赖老的 timeout 行为来猜测热帧状态

### 9.10.20 Control-plane synchronous semantic refresh was unsafe and has been rolled back (2026-04-19 14:20 +08:00)

本轮先做了一次纯排障止血，没有继续开新验证，因为上一轮后台 `War3`
已经被刷到高 CPU。

#### 9.10.20.1 What happened

1. 上一轮为了验证 `semanticCoreFrameFresh=false` 是否只是 stale summary，
   在 `src/d3d9/war3/tools/war3_control_plane.cpp` 中把：
   - `get_hot_shadow_probe`
   - 带热帧门槛的 `wait_until`
   临时改成会调用
   `render::QueryShadowRuntimeBridgeSummary(true)`。
2. 这意味着 control-plane 线程会在收到热帧探测请求时，直接同步触发：
   - `ShadowValidationRuntime::ensureFrameBuiltForContract(...)`
   - 也就是完整的 semantic build。
3. 随后隔离桌面后台样本明确出现：
   - `wait_for_game_ready -> debug-events-stalled`
   - `frameIndex=882`
   - `render.isInGame=false`
   - `get_hot_shadow_probe` 持续 `等待 control-plane 响应超时`
   - `get_shadow_runtime_summary(refreshSemanticFrameIfStale=true)`
     同样超时
4. 用户侧实机随后观察到：
   - War3 进程 CPU 直接冲高到 100%
   - 机器卡住数分钟，只能通过任务管理器强关

#### 9.10.20.2 Root-cause judgment

1. 当前可以明确判断：
   - **不能**在 control-plane 线程里同步追热帧 semantic build；
   - 至少在当前 `scene submission` 模式 + 隔离桌面 `debug-events-stalled`
     边界下，这会把 semantic build 直接放到 pipe 请求路径里。
2. 因此控制面的问题不是：
   - pipe server 本身坏了
   - 或普通 `get_shadow_runtime_summary` 本身一定会卡死
3. 真正危险的是：
   - `QueryShadowRuntimeBridgeSummary(true)`
   - 在冷帧/假热帧宿主里走同步 build
   - 再叠加 AutoTest 的轮询 hot probe
   - 会把 War3 主机拖进高 CPU / 长时间无响应状态

#### 9.10.20.3 Immediate mitigation landed

1. 已立即回撤本轮风险改动：
   - `get_hot_shadow_probe`
   - 热帧门槛版 `wait_until`
   - `get_shadow_runtime_summary(payload.refreshSemanticFrameIfStale)`
   全部恢复为只读最新已发布 semantic snapshot。
2. 对应代码：
   - `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `src/d3d9/war3/tools/war3_control_plane.h`
3. 现有注释已明确写出：
   - control-plane 不允许再直接做同步 semantic build
   - 后续若要恢复 hot-frame freshness 验证，必须改成
     **受控/限流/异步** 路线，而不是直接在 pipe handler 里 build

#### 9.10.20.4 Current blocker after rollback

1. 当前再次确认：
   - 主 blocker 仍然不是 `l1cHop`
   - 也不是 `CGeosetData` 主链本身
2. 现在真正剩下的硬骨头是：
   - 如何在**不把 control-plane 拉爆**的前提下，拿到真正的 hot-frame
     semantic summary
   - 以及在拿到热帧后，继续收：
     - `no-pose`
     - `no-identity`
3. 在没有安全 hot-frame 路线之前，禁止再在后台自动化里启用
   “同步 refresh semantic frame”的 control-plane 探测。

### 9.10.21 ShadowValidationRuntime now coalesces duplicate semantic builds (2026-04-19 14:33 +08:00)

这轮继续推进，但**没有再启动 War3**。目标是先从代码层把“重复 semantic
build 叠加导致 CPU 被打爆”的结构性风险压下去。

#### 9.10.21.1 Newly confirmed risk in code

1. `ShadowRuntimeContractCache::captureLiveState()` 在 publish 新 contract 后，
   会在锁外直接调用：
   - `ShadowValidationRuntime::ensureFrameBuiltForContract(...)`
2. 同时，之前为了追热帧，我们也尝试过在 control-plane 路径里触发同一个函数。
3. 而 `ShadowValidationRuntime::ensureFrameBuiltForContract(...)` 旧实现的行为是：
   - 只在进入函数开头做一次“是否已经是同一 frame/revision”检查；
   - 之后完整 semantic build 都在无锁区执行；
   - build 结束后才重新拿锁写回 `m_lastStats / m_lastFrame`。
4. 这意味着只要有两个线程在“上一份快照还没写回”时同时进入，就可能：
   - 对同一份 contract 做重复 build；
   - 或对一串新旧 contract 并发 build；
   - 在宿主已经 `debug-events-stalled` 的情况下，进一步放大 CPU 压力。

#### 9.10.21.2 Code hardening landed this round

1. `src/d3d9/war3/shadow/war3_shadow_renderer_core.h`
   - `ShadowValidationRuntime` 新增：
     - `m_buildInProgress`
     - `m_buildFrameSerial`
     - `m_buildPublishRevision`
     - `m_pendingManifest/resources/poses`
2. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `ensureFrameBuiltForContract(...)` 已改成：
     - **single-flight**
       同一时刻只允许一条 semantic build 真正执行；
     - **pending coalescing**
       若 build 进行中又来了更“新”的 contract，
       不再并发起第二个 build，
       而是只保留一份“最新 pending contract”；
     - **strict newer-than-current-build check**
       同一份 publishRevision/frameSerial 不会再被串行重复算第二遍；
     - build 完成后若有 pending，则在同一 worker 上继续处理最新 pending，
       否则才真正退出 build-in-progress 状态。
3. `reset()` 也同步清空：
   - build-in-progress state
   - pending contract state

#### 9.10.21.3 Why this matters

1. 这轮不是直接修好 hot-frame freshness。
2. 但它先把一个很关键的结构性风险收住了：
   - 即使以后恢复“安全的热帧刷新”
   - control-plane、runtime publish、observe validation
     也不会再轻易对同一份 semantic contract 形成
     “多线程重复全量重建”的 stampede。
3. 这一步是后续继续恢复 hot-frame 验证通道的前置安全垫。

#### 9.10.21.4 Validation completed this round

1. 本轮**没有启动 War3**，遵守了“先排查问题、不要再把机器拖爆”的约束。
2. 已完成的验证只有：
   - `ninja -C build32` 通过
3. 当前这轮结论属于：
   - 代码级风险缓解已落地
   - 运行时重新验证仍需等下一轮在更安全的探测方案下进行

### 9.10.22 Safe async hot-frame refresh path has been landed in code (2026-04-19 14:42 +08:00)

这轮继续推进，但依旧**没有启动 War3**。目标是把后续热帧验证所需的
control-plane / semantic refresh 通道改成“异步请求 + 单飞行构建”，避免
再次把宿主拖到 100% CPU。

#### 9.10.22.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_renderer_core.h/.cpp`
   - `ShadowValidationRuntime` 新增：
     - `requestLatestFrameBuild()`
     - `buildStateSnapshot()`
     - `m_lastBuildDurationUs`
   - `ensureLatestFrameBuilt()` 现在会优先消费：
     - `m_pendingManifest/resources/poses`
     而不是只能重新从 `ShadowRuntimeContractCache` 抓最新 bundle。
   - `ensureFrameBuiltForContract(...)` 现已具备：
     - single-flight
     - latest-pending coalescing
     - same-contract duplicate suppression
     - build duration 记录
2. `src/d3d9/war3/render/war3_shadow_runtime_bridge.h/.cpp`
   - `QueryShadowRuntimeBridgeSummary(true)` 的语义已被改写：
     - 不再同步触发完整 semantic build
     - 只做 `requestLatestFrameBuild()`
     - 真正的 build 仍由安全时机（如渲染路径）消费
   - runtime summary 现在新增导出：
     - `semanticCoreBuildInProgress`
     - `semanticCoreBuildRequestPending`
     - `semanticCoreBuildFrameSerial`
     - `semanticCoreBuildPublishRevision`
     - `semanticCorePendingFrameSerial`
     - `semanticCorePendingPublishRevision`
     - `semanticCoreBuildDurationUs`
3. `src/d3d9/war3/tools/war3_control_plane.h/.cpp`
   - `QueryShadowRuntimeSummary(...)` 重新支持 bool 参数
   - `get_shadow_runtime_summary`
   - `get_hot_shadow_probe`
   - 带热帧门槛的 `wait_until`
     现在都可以通过：
     - `requestSemanticFrameBuild=true`
     - 或 `refreshSemanticFrameIfStale=true`
     来发起**异步 semantic refresh 请求**
   - 但不会再在 pipe handler 中同步 build

#### 9.10.22.2 Why this matters

1. 上一轮我们已经确认：
   - “control-plane 同步 semantic refresh”是危险路径
2. 本轮的意义是：
   - 并不是简单地把这条能力永久禁掉
   - 而是把它改造成**安全的异步请求式能力**
3. 这样后续恢复后台热帧验证时：
   - probe 可以请求“请尽快拉齐最新 semantic frame”
   - 但不会再把完整重建直接压到 pipe 线程/探测线程
4. 再叠加上一轮已经落地的 single-flight/coalescing：
   - 即使短时间多次请求 hot probe
   - 也只会保留一份“最新 pending contract”
   - 不会把同一份 semantic contract 反复并发重建

#### 9.10.22.3 Validation completed this round

1. 本轮依旧**没有启动 War3**。
2. 已完成验证：
   - `ninja -C build32` 通过
3. 当前这轮仍属于：
   - 控制面/语义重建安全通道已在代码层落地
   - 运行时重新签收需要放到下一轮、并继续遵守“不要再把机器拖爆”的前提下进行

#### 9.10.22.4 AutoTest side is now wired to the safe async path

1. `AutoTest/war3_autotest_mcp.py`
   - `wait_for_hot_shadow_frame()` 调用 `get_hot_shadow_probe`
     时，现已默认携带：
     - `requestSemanticFrameBuild=true`
2. 这意味着下一轮恢复后台验证时：
   - hot probe 会主动请求“尽快拉齐最新 semantic frame”
   - 但不会再走旧的同步 semantic build 危险路径
3. 本轮已完成：
   - `python -m py_compile AutoTest/war3_autotest_mcp.py`
   - 通过

### 9.10.23 Semantic build is now publish-decoupled and request-driven end-to-end (2026-04-19 15:01 +08:00)

这轮继续推进，仍然**没有启动 War3**。目标是把 semantic build 的触发链彻底从
“publish 时同步重建”收成“只发布 contract + 请求构建，由安全时机消费”。

#### 9.10.23.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`
   - `ShadowRuntimeContractCache::captureLiveState()` 在 publish 新 contract 后，
     不再直接调用：
     - `ShadowValidationRuntime::ensureFrameBuiltForContract(...)`
   - 现在只会调用：
     - `ShadowValidationRuntime::requestLatestFrameBuild()`
   - 也就是说，`EndFrame/CaptureLiveState` 这条路径已经不再同步背负完整
     semantic build。
2. `src/d3d9/war3/shadow/war3_shadow_renderer_core.h/.cpp`
   - 新增：
     - `ShadowValidationBuildState`
     - `buildStateSnapshot()`
     - `buildDurationUs`
   - `requestLatestFrameBuild()` 现在可以把最新 published contract
     安全挂到 pending 槽。
   - `ensureLatestFrameBuilt()` 则优先消费 pending contract；
     如果没有 pending，再 fallback 到直接读取当前 bundle。
3. `src/d3d9/war3/render/war3_shadow_runtime_bridge.h/.cpp`
   - runtime summary 新增导出：
     - `semanticCoreBuildDurationUs`
     - `semanticCoreBuildInProgress`
     - `semanticCoreBuildRequestPending`
     - `semanticCoreBuildFrameSerial`
     - `semanticCoreBuildPublishRevision`
     - `semanticCorePendingFrameSerial`
     - `semanticCorePendingPublishRevision`
4. `src/d3d9/war3/tools/war3_control_plane.h/.cpp`
   - `QueryShadowRuntimeSummary(bool)` 重新显式支持参数
   - `get_shadow_runtime_summary`
   - `get_hot_shadow_probe`
   - 热帧版 `wait_until`
     现在都可以发：
     - `requestSemanticFrameBuild=true`
     但只会走**异步请求**，不会同步 build
5. `AutoTest/war3_autotest_mcp.py`
   - `wait_for_hot_shadow_frame()` 继续默认通过：
     - `requestSemanticFrameBuild=true`
     走新安全通道

#### 9.10.23.2 What this means now

1. 到本轮为止：
   - control-plane 不会同步 build
   - publish path 不会同步 build
   - hot probe 只会请求 build
   - 真正 build 由 `ensureLatestFrameBuilt()` 在安全时机消费
2. 这意味着：
   - semantic build 已经从“谁查 summary 谁可能触发重建”
   - 收敛成“请求式 + render/runtime 安全消费式”
3. 这也是恢复后台验证前必须完成的最后一层结构性止血。

#### 9.10.23.3 Validation completed this round

1. 本轮依旧**没有启动 War3**。
2. 已完成验证：
   - `ninja -C build32` 通过
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 之前已通过，当前仍保持一致
3. 当前下一步已经收敛成：
   - 在这套“异步请求 + single-flight + publish 解耦”新链上
   - 用更保守的后台方式恢复 hot-frame 验证
   - 再继续收真正剩下的：
     - `no-pose`
     - `no-identity`
     - `frameFresh` 是否能安全抬起

### 9.10.24 Control-plane now hard-gates semantic build requests behind in-game safety checks (2026-04-19 15:27 +08:00)

这轮继续推进，仍然**没有启动 War3**。目标是把上一轮新加的异步请求通道
再加一层“假热保护”，避免任何客户端在 `ready` 但还没真正进入 world render
时，持续请求 semantic build。

#### 9.10.24.1 Code changes landed this round

1. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - 新增：
     - `WantsSemanticFrameBuild(...)`
     - `ShouldRequestSemanticBuild(...)`
   - 现在 control-plane 会先读：
     - `runtimeStatus`
     - `shadowRuntimeSummary`
   - 只有满足以下条件才会真正调度 `requestLatestFrameBuild()`：
     - 调用方确实请求了 `requestSemanticFrameBuild/refreshSemanticFrameIfStale`
     - `render.isInGame == true`
       - 除非显式传 `allowPreInGameSemanticBuild=true`
     - `render.inGameRenderReady == true`
     - `runtime.gameStarted == true`
     - 当前 semantic frame 还不 fresh
     - 当前没有 `buildInProgress/buildRequestPending`
     - 若这是热帧门槛等待，还要求 `frameIndex` 已超过 `readyFrameBaseline`
2. `get_shadow_runtime_summary`
   - 不再“只要传了 request=true 就无条件调度 build”
   - 现在改成：
     - 先取快照
     - 再按安全门槛决定要不要异步请求
3. `get_hot_shadow_probe`
   - 改为：
     - 先取快照 summary
     - 再判断是否允许请求 build
   - 返回结果新增：
     - `requestedSemanticFrameBuild`
4. `wait_until`
   - 带热帧门槛时，也改成“先读 summary，再按安全门槛决定要不要调度 build”
   - 这样即便后台 probe 持续轮询，也不会在“首帧假热/非 in-game”阶段反复追帧

#### 9.10.24.2 Why this matters

1. 上一轮虽然已经把同步 build 从 control-plane 线程挪掉了，但如果客户端一直在
   `ready` 后立刻轮询 `requestSemanticFrameBuild=true`，仍可能在宿主还没真正进入
   in-game world render 时，把 pending/build 请求持续堆高。
2. 这一轮之后，semantic build 的安全门槛已经变成两层：
   - 第一层：`single-flight + latest-pending coalescing`
   - 第二层：**control-plane 只在 in-game 安全窗口内才允许请求新 build**
3. 这意味着：
   - “假热 ready”不再触发 semantic build storm
   - 恢复后台验证时可以优先观察：
     - `requestedSemanticFrameBuild`
     - `semanticCoreBuildInProgress`
     - `semanticCoreBuildRequestPending`
     - `semanticCoreFrameFresh`

#### 9.10.24.3 Validation completed this round

1. 本轮依旧**没有启动 War3**。
2. 已完成验证：
   - `ninja -C build32` 通过
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过
3. 当前下一步继续收敛成：
   - 在这个新 safety gate 上恢复**更保守**的后台 hot-frame 验证
   - 再继续收：
     - `no-pose`
     - `no-identity`
     - `semanticCoreFrameFresh`

### 9.10.25 Static semantic recovery gaps narrowed further: unitPtr propagation, worldObjectEntry-first lookup, and client-side hot-probe throttling (2026-04-19 16:02 +08:00)

这轮继续推进，仍然**没有启动 War3**。目标是直接把当前静态可见的
`no-pose / no-identity` 真缺口先补掉，而不是在恢复后台验证前继续猜。

#### 9.10.25.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `ConvertVisibleRecord(...)`
     - 现在会把 `VisibleRenderableRecord.identity.unitPtr`
       显式带入 `ShadowRenderableRecord.unitPtr`
     - 之前这一步丢失了 unit 语义，导致：
       - `ShadowRenderableRecord::hasStableIdentity()` 变弱
       - `TryResolveBestPoseForRenderable(...)` 少了一条最直接的 `findByUnitPtr`
   - `TryAugmentRenderableSemanticRecovery(...)`
     - 现在除了 `AugmentShadowSemanticContext(...)` 和 `worldInstance`
       外，还会显式从 `ShadowObjectRegistry` 继续补：
       - `worldObjectEntry`
       - `sceneNode`
       - `unitPtr`
       - `runtimeModelPtr`
       - `modelResourcePtr`
       - `modelKey`
       - `jHandle`
       - `rawcode`
       - `objectKind`
     - 同时也会从 `worldInstance` 回填 `unitPtr`
   - `TryResolveBestPoseForRenderable(...)`
     - 不再只靠 `runtimeModel / unitPtr / sceneNode / RenderObjectRegistry`
     - 现在新增：
       - `ModelInstanceRegistry` 按 `worldObjectEntry/sceneNode/unitPtr/jHandle/runtimeModel`
         回收 pose 候选
       - `ShadowObjectRegistry` 按同样的键继续回收 pose 候选
     - 这意味着即使 `RenderObjectRegistry` 当前帧没有把对象完全串起来，
       也仍有两层运行时 registry 可直接恢复 pose
2. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - `ResolveModelMetadata(...)`
     - 现在 `ModelInstanceRegistry` / `ShadowObjectRegistry`
       都先按 `worldObjectEntry` 查，再回退 `sceneNode / jHandle / unitPtr`
     - 之前这条恢复链缺了一条最自然的 key
   - `FinalizeVisibleRecord(...)`
     - 在 `ResolveModelMetadata(...)` 之前，若当前 record 的 identity 仍不完整，
       会先尝试从 prior snapshot 继承 identity
     - 这样 `ResolveModelMetadata(...)` 能在同一轮更早拿到：
       - `unitPtr`
       - `jHandle`
       - `rawcode`
       - `kind`
3. `AutoTest/war3_autotest_mcp.py`
   - `wait_for_hot_shadow_frame(...)`
     - 不再每 0.5s 都固定发送 `requestSemanticFrameBuild=true`
     - 现在只有在以下条件同时满足时才会请求：
       - `render.isInGame == true`
       - `render.inGameRenderReady == true`
       - `runtime.gameStarted == true`
       - 当前 `semanticCoreFrameFresh == false`
       - 当前没有 `semanticCoreBuildInProgress`
       - 当前没有 `semanticCoreBuildRequestPending`
       - `frameIndex > readyFrameBaseline`
       - 距离上次真正发 build 请求至少 1 秒
     - control-plane 端和客户端现在都具备节流，恢复后台验证时更安全

#### 9.10.25.2 Why this matters

1. 这轮不是再做“安全通道基础设施”，而是直接补当前 semantic 链剩下的真实恢复缺口：
   - `unitPtr` 传播
   - `worldObjectEntry` 优先恢复
   - `pose` 直接从 instance/shadow registries 回收
2. 这三点都直接针对当前剩余 blocker：
   - `semanticCoreSkippedNoPose`
   - `semanticCoreSkippedNoIdentity`
3. 同时，客户端热帧轮询也不再盲目追帧；恢复后台验证时，CPU 风险进一步降低

#### 9.10.25.3 Validation completed this round

1. 本轮依旧**没有启动 War3**。
2. 已完成验证：
   - `ninja -C build32` 通过
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过
3. 下一步继续收敛成：
   - 在新安全门槛 + 新恢复链上恢复更保守的后台热帧验证
   - 直接观察：
     - `semanticCoreSkippedNoPose`
     - `semanticCoreSkippedNoIdentity`
     - `semanticCoreFrameFresh`

### 9.10.26 Static recovery chain tightened further: sprite-mediated pose/resource recovery and prior-manifest inheritance by mesh/geoset keys (2026-04-20 11:25 +08:00)

这轮继续在**不启动 War3** 的前提下往剩余语义链缺口上收，目标是先把
`no-pose / no-identity` 的静态漏链再压一层，再恢复更保守的后台热帧验证。

#### 9.10.26.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - 新增：
     - `TryResolveSpritePtrFromUnit(...)`
     - `TryReadRuntimeModelFromSprite(...)`
   - `ConvertPoseRecord(...)`
     - 现在会把 `model::PoseRecord.unitPtr`
       正式带进 `ShadowPoseRecord.unitPtr`
     - 之前这条链在 `ShadowRendererCore` 本地转换时会丢失单位维度，
       会削弱 `findByUnitPtr(...)`
   - `TryAugmentRenderableSemanticRecovery(...)`
     - `needsAugment` 现在不再只看 `runtimeModel/modelResource/jHandle/rawcode`
     - 只要 `worldObjectEntry / sceneNode / unitPtr` 有缺口，也会继续尝试补全
     - 新增 `sprite` 作为中介键：
       - 通过 `unit -> sprite`
       - 再走 `ModelInstanceRegistry.findBySpritePtr(...)`
       - 再走 `ShadowObjectRegistry.findBySpritePtr(...)`
       - 必要时直接 `sprite -> runtimeModel`
       - 同时也会通过 `ModelRegistry.findBySprite(...)`
         回补 `modelResource/modelKey`
   - `TryResolveBestPoseForRenderable(...)`
     - 现在除了 `runtimeModel/unitPtr/sceneNode/worldObjectEntry/jHandle`
       外，也会显式使用：
       - `sprite -> runtimeModel -> poses`
       - `instanceRegistry.findBySpritePtr(...)`
       - `shadowRegistry.findBySpritePtr(...)`
     - 这意味着：即使当前 record 只有 `unit` 或者只剩 `sprite` 血缘，
       也仍能更直接地恢复 pose
   - `no-pose detail` 诊断
     - 现在日志里会显式打印：
       - `sprite`
       - `instanceBySprite`
       - `sprite -> runtimeModel`
     - 后续恢复运行时验证时，能够更快判断缺口到底卡在
       `unit -> sprite`、`sprite -> runtimeModel`，
       还是 registry 本身
2. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - `ResolveModelMetadata(...)`
     - 在 `unit -> sprite` 之后，新增：
       - `ModelInstanceRegistry.findBySpritePtr(...)`
       - `ShadowObjectRegistry.findBySpritePtr(...)`
     - 并在命中时直接回填：
       - `runtimeModelPtr`
       - `modelResourcePtr`
       - `modelKey`
     - 这条补链针对的是当前已知的 `resource-miss/no-pose`
       场景里，“有 unit/sprite，但 scene/world/key 还没完全回正”的记录
3. `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`
   - `FindPriorRenderableRecord(...)`
     - 现在除了 `world/scene/unit/runtime/handle/renderablePart/payload`
       外，还会用：
       - `meshData`
       - `runtimeGeosetPtr`
       - `runtimeGeosetDataPtr`
       去匹配 prior manifest
   - `MergeRenderableIdentityFromPrior(...)`
     - 现在也会一并继承：
       - `runtimeGeosetPtr`
       - `runtimeGeosetDataPtr`
       - `meshData`
       - `geosetIndex`
     - 这样匿名子部件如果跨帧保持稳定，也更容易把身份/资源上下文继承回来
   - `RepairManifestIdentityFromPrior(...)`
     - “已经有强身份”的判断也收严了：
       - 现在不仅要求 `kind/rawcode/jHandle` 齐，
       - 还要求至少已有 `worldObjectEntry/sceneNode/unitPtr/runtimeModel`
         其中之一
     - 避免“只有半截 handle/rawcode，但上下文仍丢失”的 record 过早跳过修补

#### 9.10.26.2 Why this matters

1. 剩余 `no-pose / no-identity` 的静态根因，已经越来越像：
   - 某些 record 不是完全没线索，
   - 而是当前代码还没充分用到 `sprite` 和
     `mesh/runtimeGeosetData` 这类稳定中介键
2. 这轮之后：
   - `pose` 恢复不再局限于 `runtimeModel/unit/scene`
   - `metadata` 恢复也不再只靠 `world/scene/handle/unit`
   - prior-manifest 的身份修补能沿 `meshData/runtimeGeosetData`
     这类稳定子部件继续继承
3. 这为下一步恢复后台热帧验证创造了更好的静态基础：
   - 如果 `no-pose/no-identity` 仍然高，就更可能是 runtime 真实缺口，
     而不是静态恢复链没写完整

#### 9.10.26.3 Validation completed this round

1. 本轮依旧**没有启动 War3**。
2. 已完成验证：
   - `ninja -C build32` 通过
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过
3. 当前下一步继续收敛成：
   - 在现有 safety gate 下恢复**更保守**的后台热帧验证
   - 优先观察：
     - `semanticCoreSkippedNoPose`
     - `semanticCoreSkippedNoIdentity`
     - `semanticCoreFrameFresh`
     - 新增 `sprite` 相关 no-pose detail 是否开始命中

### 9.10.27 Phase 4A closed and Phase 4B runtime hot-frame validation restored on the new safety path (2026-04-21 13:52:27 +08:00)

这轮已经从“静态补链 + 安全门准备”推进到真正的实机签收。

#### 9.10.27.1 Code changes landed this round

1. `AutoTest/war3_autotest_mcp.py`
   - `wait_for_game_ready(...)`
     - 修正为在 `timeout_sec` 窗口内等待 `\\.\pipe\War3ControlPlane_<pid>` 出现
     - `require_control_plane_ready=true` 时仍然只认 pipe 成功，但不再在 pipe 尚未创建的瞬间误判失败
2. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `ShadowValidationRuntime::ensureLatestFrameBuilt()`
     - 不再在 build 进行中用更新 manifest 抢断当前 work
     - 当前策略改成：
       - 当前 build 单飞推进
       - 最新 contract 保留在 `m_pending*`
       - 当前 build 完成后再切下一轮
   - semantic chunk budget 从 `50ms` 提到 `100ms`
     - 仍保持 chunked build，不退回同步全量构建
3. `src/d3d9/war3/model/war3_model_resource_cache.cpp`
   - `noteRuntimeModelBinding(...)`
     - 即使暂时读不到 runtime geoset 数组，也保留最小 runtime record
     - 修掉了 `shadowRuntimeModelCount` 长期为 `0` 的次级索引断链
4. `src/d3d9/war3/render/war3_visible_renderables.cpp`
   - `ResolveModelMetadata(...)`
     - 当前帧已经拿到的 `runtimeModelPtr/modelResourcePtr/modelKey`
       会反向回填 `ShadowModelResourceCache`
5. `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`
   - `captureLiveState()`
     - 同 `frameSerial` 的 duplicate/regression capture 不再重复 bump `publishRevision`
6. `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
   - `semanticCoreFrameFresh`
     - 从“`publishRevisionLag == 0` 才 fresh”
       调整成“至多落后一帧，且同帧 publish lag 受控”
     - 避免把同帧重复 publish 放大的“假陈旧”继续误判为 stale

#### 9.10.27.2 Phase completion timestamps and evidence

1. `Phase 4A` 完成时间：
   - `2026-04-21 13:52:27 +08:00`
   - 证据：
     - 隔离桌面 `model_runtime_probe`
       - `AutoTest/artifacts/model_runtime_probe_latest.json`
       - `ready.mode=control-plane`
       - `hotShadow.ok=true`
       - report:
         - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_13_47_44.html`
     - 普通桌面 `model_runtime_probe` ready
       - `AutoTest/artifacts/model_runtime_probe_normal_ready_latest.json`
       - `ready.ok=true`
       - `ready.mode=control-plane`
2. `Phase 4B` 当前轮完成时间：
   - `2026-04-21 13:50:45 +08:00`
   - 三套命名场景全部通过 control-plane hot-frame 链：
     - `AutoTest/artifacts/model_runtime_probe_latest.json`
       - `shadowRuntimeModelCount=179`
       - `semanticCoreResolved=135`
       - `semanticCoreSkinnedResolved=135`
       - `semanticCoreFrameFresh=true`
     - `AutoTest/artifacts/low_pressure_static_reuse_latest.json`
       - `semanticCoreFrameFresh=true`
       - report:
         - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_13_49_09.html`
     - `AutoTest/artifacts/dynamic_shadow_pressure_latest.json`
       - `semanticCoreFrameFresh=true`
       - `semanticCoreResolved=123`
       - `semanticCoreSkinnedResolved=123`
       - report:
         - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_13_50_16.html`

#### 9.10.27.3 What is now actually true

1. `Phase 4A`
   - 已从“代码层有 pipe / safety gate”推进到“隔离桌面 + 普通桌面都能 stable ready”
   - `wait_for_game_ready` 默认 acceptance 现在真正只认 control-plane 成功
2. `Phase 4B`
   - 新 hot-frame 验证链已经恢复
   - `shadowRuntimeModelCount` 已从 `0` 拉到 `179`
   - `semanticCoreFrameFresh` 已能在三套命名场景下重新成立
3. 当前仍未完成的不是 4A/4B，而是：
   - `Phase 4C`: DXVK host backend 真正从 `D3D9DeviceEx` 抽成独立后端
   - `Phase 4D`: perf/report 指标与 mixed-mode 成本彻底收口
   - `Phase 5`: native D3D9 / late-inject

#### 9.10.27.4 Current blockers after Phase 4A / 4B closeout

1. `shadowModelResourceCount` 仍然是 `0`
   - runtime lineage 已恢复，但 direct `modelResource` 根对象还没成为稳定事实源
2. `semanticCoreSkippedNoPose` / `semanticCoreSkippedNoRuntimeGroupPalette`
   - 已不再阻塞 hot-frame 签收
   - 但仍是后续 correctness / shape 收口的主要残余缺口
3. `Phase 4C/5`
   - 当前 object shadow 主路径仍主要在：
     - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket(...)`
     - `D3D9DeviceEx::War3TryPopulateSemanticShadowScene(...)`
   - 还没有完成真正的 backend cutover

### 9.10.28 Phase 4C advanced: DXVK semantic shadow submission now runs through `ShadowRendererCore -> DxvkValidationBackend -> DXVK host adapter` (2026-04-21 14:11:47 +08:00)

这轮没有误报 `Phase 4C` 已完成，但已经把最核心的一步真正前推了：

1. `War3TryPopulateSemanticShadowScene(...)`
   - 不再自己遍历 `frame->draws` 再逐包调用 `War3TryAppendSemanticShadowPacket(...)`
   - 当前改成：
     - `ShadowRendererCore::submitFrame(...)`
     - `DxvkValidationBackend`
     - `DXVK host adapter`
2. `DxvkValidationBackend`
   - 不再只是 hash-only 骨架
   - 现在会：
     - 通过宿主接口决定 packet 是否进入 object shadow 提交
     - 在 submit 成功后记录：
       - `normalized handle`
       - `worldObjectEntry`
       - `sceneNode`
       - `runtimeModelPtr`
   - 这些 identity set 再回流给 fallback prune
3. `D3D9DeviceEx`
   - 当前仍保留 DXVK 资源上传/scene draw 宿主能力
   - 但语义帧的“逐包编排权”已经不再留在设备层主循环里

#### 9.10.28.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_backend_dxvk.h`
   - 新增 `IDxvkValidationHost`
   - `DxvkValidationBackend` 新增：
     - `configureHost(...)`
     - submitted identity accessors
2. `src/d3d9/war3/shadow/war3_shadow_backend_dxvk.cpp`
   - backend 现在支持：
     - host-side submit filter
     - host-side packet submit
     - 成功提交后的 identity 归集
3. `src/d3d9/d3d9_device.h`
   - `D3D9DeviceEx` 持有持久 `m_war3SemanticDxvkBackend`
4. `src/d3d9/d3d9_device.cpp`
   - `War3TryPopulateSemanticShadowScene(...)`
     - 当前改为通过 `ShadowRendererCore::submitFrame(...)`
       驱动 `m_war3SemanticDxvkBackend`
     - fallback prune 改为消费 backend 的 submitted identity 集
   - `semanticFallbackPruned*`
     - 已开始在 prune 路径上真实累计
     - 后续 perf/report 可以直接观测 semantic 提交对 legacy fallback 的清退量

#### 9.10.28.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 三套命名场景重新通过当前 DXVK validation 路线：
   - `model_runtime_probe`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreFrameLag=1`
     - `semanticCorePublishRevisionLag=11`
     - `shadowRuntimeModelCount=179`
     - `matrixPaletteCount=182`
     - `shadowReadyGeosetCount=2421`
     - `semanticCoreResolved=122`
     - `semanticCoreSkinnedResolved=122`
   - `low_pressure_static_reuse`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreFrameLag=0`
     - `semanticCorePublishRevisionLag=0`
     - `shadowRuntimeModelCount=59`
     - `matrixPaletteCount=41`
     - `shadowReadyGeosetCount=1993`
     - `semanticCoreResolved=32`
     - `semanticCoreSkinnedResolved=32`
   - `dynamic_shadow_pressure`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreFrameLag=1`
     - `semanticCorePublishRevisionLag=10`
     - `shadowRuntimeModelCount=179`
     - `matrixPaletteCount=182`
     - `shadowReadyGeosetCount=2307`
     - `semanticCoreResolved=120`
     - `semanticCoreSkinnedResolved=120`
3. 最新报告：
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_14_08_35.html`
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_14_10_03.html`
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_14_11_12.html`
4. 追加 sanity run（prune 计数补丁后）：
   - `model_runtime_probe`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `shadowRuntimeModelCount=179`
     - `semanticCoreResolved=120`
     - `semanticCoreSkinnedResolved=120`

#### 9.10.28.3 What is now actually true

1. 当前 DXVK validation backend 已经不只是“compile-time contract 骨架”
2. 语义 object shadow 提交编排已经真正经过：
   - `ShadowRendererCore`
   - `DxvkValidationBackend`
   - DXVK host adapter
3. 这轮切换后，三套场景没有把：
   - control-plane ready
   - hot-frame validation
   - runtime lineage
   重新打坏

#### 9.10.28.4 Why Phase 4C is still not closed

1. `War3TryAppendSemanticShadowPacket(...)`
   - 仍然承载 DXVK 侧真实资源上传与 scene submission
   - 设备层还没有收缩到纯宿主适配壳
2. `War3AllocFreezeBuffer`
   - 仍然在新的 object shadow 提交链里出现
   - 说明 frame-local dynamic upload / old allocator 依赖还没退出主路径
3. `SnapshotFallback`
   - 仓库内仍然存在 legacy 诊断/兼容代码
   - 还没有完成“只留 emergency diagnostic mode，不再触达主 object shadow 路径”的彻底清退
4. 因此当前最准确的状态不是“4C 完成”，而是：
   - `4C` 的 submission orchestration 已完成切换
   - `4C` 的宿主壳清退与 legacy allocator/snapshot 清退还没完成

### 9.10.29 Phase 4D advanced: report/control-plane now expose semantic persistent-vs-frame-local submission, and default mainline no longer allows frame-local dynamic upload (2026-04-21 15:37:35 +08:00)

这轮继续往 `Phase 4D` 推进，但没有误报成完成。

#### 9.10.29.1 Code changes landed this round

1. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`
   - 新增 `semanticSceneSubmittedPersistent`
   - control-plane / diagnostics / runtime summary 都能直接看到：
     - `semanticSceneSubmitted`
     - `semanticSceneSubmittedFrameLocal`
     - `semanticSceneSubmittedPersistent`
2. `src/d3d9/war3/tools/war3_perf_monitor.*`
   - perf report 现在正式输出：
     - `semanticSceneSubmittedFrameLocal`
     - `semanticSceneSubmittedPersistent`
   - 不再需要手工拿 `submitted - frameLocal` 反推
3. `AutoTest/war3_autotest_mcp.py`
   - 读取 perf report 时同步纳入：
     - `semanticSceneSubmitted`
     - `semanticSceneSubmittedUnit`
     - `semanticSceneSubmittedSkinned`
     - `semanticSceneSubmittedFrameLocal`
     - `semanticSceneSubmittedPersistent`
4. `src/d3d9/war3/core/war3_internal_test_config.h`
   - 新增：
     - `kShadowSemanticCoreAllowFrameLocalDynamicGeometry = false`
   - 默认主线不再允许 semantic scene 回落到 frame-local dynamic mesh upload
   - 如需紧急诊断旧动态流，才临时打开
5. `src/d3d9/d3d9_device.cpp`
   - semantic scene 当前如果命中 `packet.usesDynamicMeshPositions`
     且未显式放开诊断开关，就直接 reject
   - 避免主线悄悄回退到：
     - `War3AllocFreezeBuffer`
     - per-draw host upload

#### 9.10.29.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. `python -m py_compile AutoTest/war3_autotest_mcp.py`
   - 通过
3. 主线默认禁止 frame-local dynamic upload 后，三套场景重新通过：
   - `model_runtime_probe`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticSceneSubmitted=1300`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=1300`
     - `fallbackDrawCount=611`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_15_34_48.html`
   - `low_pressure_static_reuse`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticSceneSubmitted=868`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=868`
     - `fallbackDrawCount=1041`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_15_35_51.html`
   - `dynamic_shadow_pressure`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticSceneSubmitted=1760`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=1760`
     - `fallbackDrawCount=705`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_15_36_59.html`

#### 9.10.29.3 What is now actually true

1. acceptance 场景当前已经可以直接证明：
   - semantic scene submission 正在真实发生
   - 这些提交不是 frame-local dynamic upload
   - 当前三套场景的 semantic object shadow 提交都落在 persistent path
2. 这意味着：
   - `War3AllocFreezeBuffer` 虽然代码仍在仓库里
   - 但在当前 acceptance 主线上已经不再被 semantic scene 默认使用

#### 9.10.29.4 Why Phase 4D is still not closed

1. `fallbackDrawCount`
   - 仍明显不为 `0`
   - `low_pressure_static_reuse=1041`
   - `dynamic_shadow_pressure=705`
   - `model_runtime_probe=611`
2. 因此当前不能把结论写成：
   - “mixed-mode 成本已清零”
   - “legacy object fallback 已退出正常流”
3. 当前更准确的说法是：
   - per-draw host upload 观测与默认封堵已前推完成
   - 但 legacy fallback 清退还没完成，`Phase 4D` 仍在进行中

### 9.10.30 Phase 4D refined: object fallback is now explicitly separated from terrain fallback in report/control-plane, and current object fallback is zero on the semantic object-shadow path (2026-04-21 15:44:55 +08:00)

#### 9.10.30.1 Code changes landed this round

1. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`
   - 新增 `objectFallbackDrawCount`
   - 定义固定为：
     - `fallbackDrawCountWorldObject + fallbackDrawCountUnitObject`
2. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - control-plane summary 现在直接输出 `objectFallbackDrawCount`
3. `src/d3d9/war3/tools/war3_perf_monitor.*`
   - perf report 现在输出：
     - `fallbackDrawCountTerrain`
     - `fallbackDrawCountWorldObject`
     - `fallbackDrawCountUnitObject`
     - `objectFallbackDrawCount`
4. `AutoTest/war3_autotest_mcp.py`
   - 读取 perf report 时已同步纳入上面这些 breakdown 字段

#### 9.10.30.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. `python -m py_compile AutoTest/war3_autotest_mcp.py`
   - 通过
3. `model_runtime_probe` 追加验证：
   - `ok=true`
   - `readyOk=true`
   - `hotShadowOk=true`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_15_44_12.html`
   - report summary:
     - `fallbackDrawCount=658`
     - `fallbackDrawCountTerrain=546`
     - `fallbackDrawCountWorldObject=0`
     - `fallbackDrawCountUnitObject=0`
     - `objectFallbackDrawCount=0`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=1421`

#### 9.10.30.3 What is now actually true

1. 当前 acceptance 主线上：
   - semantic object shadow submission 正在真实发生
   - object fallback 当前已经是 `0`
   - 剩余拖高 `fallbackDrawCount` 的主要是 terrain fallback
2. 这意味着当前不能再把：
   - `fallbackDrawCount` 总数
   直接等同于：
   - object shadow fallback 仍然很多
3. 当前更准确的状态是：
   - object shadow path 已经不再走 frame-local upload
   - object fallback 当前已被压到 `0`
   - 但 terrain / 非 object fallback 仍然存在，因此总 fallback 还高

### 9.10.31 Phase 5A advanced: `NativeD3D9Backend` is no longer a handle-only stub and now owns real native D3D9 upload/cache state (2026-04-21 16:32:24 +08:00)

#### 9.10.31.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_backend_native_d3d9.h`
   - `NativeD3D9Backend` 新增：
     - `setDevice(...)`
     - `hasDevice()`
     - `frameSerial()`
     - `submittedDrawCount()`
     - `geometryCount()`
     - `paletteCount()`
     - `materialCount()`
     - `reset()`
   - backend 内部状态不再只是三张 `key -> handle` map
   - 现在会真实持有：
     - native `IDirect3DVertexBuffer9`
     - native `IDirect3DIndexBuffer9`
     - palette cache
     - material cache
     - submission record queue
2. `src/d3d9/war3/shadow/war3_shadow_backend_native_d3d9.cpp`
   - `ensureGeometry(...)`
     - 现在会从 `ShadowDrawPacket` 解析 position/index/blend 数据
     - 并创建 native `CreateVertexBuffer(...) / CreateIndexBuffer(...)`
     - geometry 不再只是分配一个递增 handle
   - `ensurePalette(...)`
     - 现在会缓存：
       - skinned `runtimeGroupPalette`
       - rigid `worldTransform` / pose 首矩阵
     - identity palette 统一固定为保留 handle `1`
   - `ensureMaterial(...)`
     - 现在会缓存真实 `ShadowMaterialSignature`
   - `submitDraw(...)`
     - 不再只是检查 handle 非零
     - 现在会校验 geometry/palette/material 资源存在
     - 并记录 native backend submission record，供后续 late-inject 执行层消费
3. native backend 约束
   - 新实现只依赖 `IDirect3DDevice9`
   - 本轮没有引入：
     - `IDirect3DDevice9Ex`
     - `D3D9DeviceEx`
     - `War3AllocFreezeBuffer`

#### 9.10.31.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 代码搜索确认：
   - `war3_shadow_backend_native_d3d9.cpp`
     - 出现真实 native 资源创建：
       - `CreateVertexBuffer`
       - `CreateIndexBuffer`
   - `war3_shadow_backend_native_d3d9.h/.cpp`
     - 未出现：
       - `IDirect3DDevice9Ex`
       - `D3D9DeviceEx`
       - `War3AllocFreezeBuffer`

#### 9.10.31.3 What is now actually true

1. `NativeD3D9Backend`
   - 已经从“纯 handle stub”推进到“真实 native D3D9 资源缓存后端”
   - 当前已经能：
     - 持有 native `VB/IB`
     - 持有 blend stream
     - 持有 palette/material cache
     - 记录 submission queue
2. 这意味着：
   - `Phase 5A` 不再是完全未开始
   - native backend 侧的 contract 已经有真实资源语义，不再只是空骨架
3. 但这还不能算 `Phase 5A` 完成，因为：
   - late-inject / native bootstrap 还没把这套 backend 真正接到运行时执行链
   - 当前还没有 native D3D9 下的 object shadow runtime draw 验证

### 9.10.32 Phase 5A advanced again: native backend now has a runtime driver entry and control-plane-visible summary surface (2026-04-21 16:37:54 +08:00)

#### 9.10.32.1 Code changes landed this round

1. 新增 `src/d3d9/war3/shadow/war3_shadow_native_runtime.h/.cpp`
   - 新增 `NativeD3D9BackendRuntime`
   - 职责：
     - 持有 `NativeD3D9Backend`
     - 消费 `ShadowValidationRuntime` 发布的 `ShadowSubmissionFrame`
     - 在设备已绑定时驱动 `ShadowRendererCore::submitFrame(...)`
     - 产出 native backend summary
2. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`
   - `ShadowRuntimeBridgeSummary` 现在新增：
     - `nativeD3D9BackendFrameSerial`
     - `nativeD3D9BackendSourcePublishRevision`
     - `nativeD3D9BackendSubmittedDrawCount`
     - `nativeD3D9BackendGeometryCount`
     - `nativeD3D9BackendPaletteCount`
     - `nativeD3D9BackendMaterialCount`
     - `nativeD3D9BackendHasDevice`
   - `QueryShadowRuntimeBridgeSummary(...)`
     - 现在会顺带刷新/snapshot native backend runtime
   - `ResetShadowRuntimeBridgeState()`
     - 现在会同步 reset native backend runtime
3. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `get_runtime_status` / `get_shadow_runtime_summary`
     - 现在会直接输出 native backend summary 字段
4. `src/d3d9/meson.build`
   - 已纳入 `war3_shadow_native_runtime.cpp`

#### 9.10.32.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 代码搜索确认：
   - 新 runtime 驱动已接到：
     - `war3_shadow_runtime_bridge.cpp`
     - `war3_control_plane.cpp`
   - control-plane 现已可直接暴露 native backend summary 字段

#### 9.10.32.3 What is now actually true

1. native 路线当前不再只是“有个 backend 类”
2. 现在已经有：
   - 一个 runtime driver 入口
   - 一条 control-plane 可观测摘要链
3. 这意味着下一步不再需要先补“如何从 semantic frame 喂给 native backend”这层胶水
4. 当前剩余 blocker 更集中为：
   - 谁来绑定真实 `IDirect3DDevice9`
   - 谁来在晚注入帧循环里真正执行 native draw

### 9.10.33 Phase 5A prep advanced: runtime bootstrap now exposes native backend bind/drive entry points (2026-04-21 16:39:59 +08:00)

#### 9.10.33.1 Code changes landed this round

1. `src/d3d9/war3/platform/war3_runtime_bootstrap.h/.cpp`
   - 新增：
     - `BindNativeShadowDevice(IDirect3DDevice9* device)`
     - `DriveNativeShadowBackend()`
2. 这两个入口当前职责固定为：
   - 把晚注入拿到的 native `IDirect3DDevice9` 绑定给 `NativeD3D9BackendRuntime`
   - 驱动 native backend 消费当前最新 semantic submission frame

#### 9.10.33.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 代码搜索确认：
   - `war3_runtime_bootstrap.h/.cpp`
     - 已出现公开 native backend bind/drive 入口

#### 9.10.33.3 What is now actually true

1. late-inject / native 路线现在已经不缺“公开挂点”
2. 下一步真正还缺的是：
   - 谁来在晚注入宿主里拿到真实设备
   - 谁来在合适帧时机调用 `DriveNativeShadowBackend()`

### 9.10.34 Phase 5A advanced again: current host now binds the native backend device and drives native runtime submission on the render thread (2026-04-21 18:27:30 +08:00)

#### 9.10.34.1 Code changes landed this round

1. `src/d3d9/d3d9_war3_hook.cpp`
   - `War3Hook::InstallHooks(IDirect3DDevice9* device)`
     - 现在会在保存 `s_device` 后立刻调用：
       - `dxvk::war3::platform::BindNativeShadowDevice(device)`
   - 这意味着当前宿主已经会把实际 `IDirect3DDevice9*` 绑定给 native backend runtime
2. `src/d3d9/d3d9_device.cpp`
   - `War3TryPopulateSemanticShadowScene(...)`
     - DXVK semantic shadow 提交完成后，当前会继续调用：
       - `dxvk::war3::platform::DriveNativeShadowBackend()`
   - native backend 不再只在 control-plane 查询时被动刷新
   - 当前已经会在真实 render thread 上消费最新 semantic submission frame

#### 9.10.34.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 三套命名场景追加实机验证：
   - `model_runtime_probe`
     - `ok=true`
     - `hotShadow.ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `nativeD3D9BackendGeometryCount=115`
     - `nativeD3D9BackendPaletteCount=94`
     - `nativeD3D9BackendMaterialCount=94`
     - `semanticCoreSubmittedDrawCount=119`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_18_24_30.html`
   - `low_pressure_static_reuse`
     - `ok=true`
     - `hotShadow.ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=33`
     - `nativeD3D9BackendGeometryCount=29`
     - `nativeD3D9BackendPaletteCount=19`
     - `nativeD3D9BackendMaterialCount=18`
     - `semanticCoreSubmittedDrawCount=33`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_18_25_43.html`
   - `dynamic_shadow_pressure`
     - `ok=true`
     - `hotShadow.ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `nativeD3D9BackendGeometryCount=115`
     - `nativeD3D9BackendPaletteCount=94`
     - `nativeD3D9BackendMaterialCount=94`
     - `semanticCoreSubmittedDrawCount=119`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_18_26_47.html`

#### 9.10.34.3 What is now actually true

1. 当前宿主已经能把真实 `IDirect3DDevice9*` 绑定到 native backend runtime
2. 当前宿主已经能在 render thread 上驱动 native backend 消费 semantic frame
3. `nativeD3D9BackendSubmittedDrawCount`
   - 当前在三套场景下都大于 `0`
   - 并且与 `semanticCoreSubmittedDrawCount` 对齐
4. 这意味着：
   - `Phase 5A` 已经从“有 backend + 有公开挂点”推进到“当前宿主内可真实驱动 native runtime submission”
5. 但这还不能算 `Phase 5B` 完成，因为：
   - 现在仍然只是“当前宿主驱动 native backend submission”
   - 还没有晚注入独立宿主下的 native object shadow 出图验收

### 9.10.35 Phase 5A advanced: native backend now has a prepared-frame execute path, but current host still keeps execution unwired from the real shadow-pass timing (2026-04-21 20:30:56 +08:00)

#### 9.10.35.1 Code changes landed this round

1. `src/d3d9/war3/shadow/war3_shadow_backend_native_d3d9.h/.cpp`
   - `NativeD3D9Backend`
     - 新增：
       - `executePreparedDraws()`
       - `executedDrawCount()`
       - `executedFrameSerial()`
   - backend 现在不再只是：
     - 上传 geometry/palette/material
     - 记录 submission queue
   - 而是已经具备：
     - rigid record 直接用 native `IDirect3DDevice9` draw
     - skinned record 基于 prepared packet/runtime group palette 执行最小 native draw
   - 当前执行层仍是“prepared-frame execute 能力”，不是最终性能签收形态
2. `src/d3d9/war3/shadow/war3_shadow_native_runtime.h/.cpp`
   - `NativeD3D9BackendRuntime`
     - 新增：
       - `executePreparedFrame()`
   - summary 现在新增：
     - `executedFrameSerial`
     - `executedDrawCount`
3. `src/d3d9/war3/platform/war3_runtime_bootstrap.h/.cpp`
   - 新增：
     - `ExecuteNativeShadowBackendPreparedFrame()`
   - 当前 bootstrap API 已明确拆成：
     - `DriveNativeShadowBackend()` 负责 build/prepare
     - `ExecuteNativeShadowBackendPreparedFrame()` 负责在正确时机执行 native draw
4. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`
   - `ShadowRuntimeBridgeSummary`
     - 新增：
       - `nativeD3D9BackendExecutedFrameSerial`
       - `nativeD3D9BackendExecutedDrawCount`
5. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `get_runtime_status` / `get_shadow_runtime_summary`
     - 已直接输出上述 execute summary 字段

#### 9.10.35.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 三套命名场景回归：
   - `model_runtime_probe`
     - `ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=119`
     - `objectFallbackDrawCount=0`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_20_27_51.html`
   - `low_pressure_static_reuse`
     - `ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=33`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=33`
     - `objectFallbackDrawCount=0`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_20_28_53.html`
   - `dynamic_shadow_pressure`
     - `ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=117`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=117`
     - `objectFallbackDrawCount=0`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_20_29_59.html`

#### 9.10.35.3 What is now actually true

1. native 路线已经不只是“能 build submission”
2. 现在已经有：
   - native prepared-frame execute 能力
   - runtime / bootstrap 的显式 execute 入口
   - control-plane 可观测 execute counters
3. 但当前宿主仍然没有在真正 native shadow-pass 时机调用：
   - `ExecuteNativeShadowBackendPreparedFrame()`
4. 因此 `nativeD3D9BackendExecutedDrawCount` 当前仍然为 `0`
5. 这说明当前最直接的 blocker 已进一步收敛成：
   - 谁来在晚注入独立宿主的真实 shadow-pass 时机调用 execute
   - 调用后做第一次 native object shadow 出图验收

### 9.10.36 Phase 5B prep advanced: native renderer hook path now has a real shadow-pass execute timing, but the path remains disabled by default (2026-04-21 20:30:56 +08:00)

#### 9.10.36.1 Code changes landed this round

1. `src/d3d9/war3/native/war3_native_renderer.cpp`
   - 已新增：
     - `dxvk::war3::platform::DriveNativeShadowBackend()`
       - 在 native shadow-pass window 前构建/prepare submission
     - `dxvk::war3::platform::ExecuteNativeShadowBackendPreparedFrame()`
       - 在：
         - `CWorld_ToggleGroup1ShadowPass(world, 1)`
         - `Stage12_Group1`
         - `Stage11_TerrainShadow12_Group0`
       - 之后、`RenderQueue_FlushAndReset()` 之前执行
2. 这意味着 native/late-inject 路线已经不再只缺“execute API”
   - 现在连“真实 shadow-pass timing”也已经在 native renderer hook 路线上有了明确接线点
3. 但该路径当前仍受：
   - `kNativeRendererHookTakeoverEnabled = false`
   - 控制

#### 9.10.36.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. 追加当前主线回归：
   - `model_runtime_probe`
     - `ok=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=117`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=117`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_20_34_34.html`

#### 9.10.36.3 What is now actually true

1. native renderer hook 路线已经具备：
   - build/prepare timing
   - execute timing
2. 当前主线场景中 `executedDrawCount` 仍为 `0`
   - 不是 execute 入口不存在
   - 而是因为 native hook takeover 默认仍关闭
3. 因此下一步 blocker 已进一步缩小为：
   - 晚注入宿主真正启用 native hook/timing
   - 第一次 native execute 出图验收

### 9.10.37 Phase 5B runtime update: native takeover now installs after semantic warmup, but render-thread scene submission is still consuming an empty semantic frame (2026-04-21 22:19:49 +08:00)

#### 9.10.37.1 Code changes landed this round

1. native takeover install trigger
   - `src/d3d9/d3d9_war3_hook.cpp`
   - `src/d3d9/d3d9_war3_hook.h`
   - `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
   - `War3Hook::MaybeInstallNativeRendererTakeover(...)` 现在不再只挂在 DXVK semantic submit 旁边
   - 新增 runtime-bridge warm gate：
     - `runtimeChainWarm=true`
     - `semanticCoreFrameFresh=true`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount>0`
2. native hook patch safety
   - `src/d3d9/war3/native/war3_native_hooks.cpp`
   - `WriteJump/WriteCall/WriteNop` 现已补 `FlushInstructionCache(...)`
3. host-side validation plumbing
   - `src/d3d9/war3/core/war3_internal_test_config.h`
   - `src/d3d9/d3d9_device.cpp`
   - 新增：
     - `kNativeRendererHostExecuteValidationEnabled`
     - `kShadowSemanticCoreSceneBootstrapCatchupEnabled`
     - `kShadowSemanticCoreSceneBootstrapCatchupMaxAttempts`
   - 目标不是直接宣称 late-inject done，而是先验证：
     - render-thread semantic scene submit 是否能拿到非空 submission frame
     - native backend prepared execute 是否有机会真正进入 draw
4. runtime diagnostics
   - `src/d3d9/d3d9_device.cpp`
   - `src/d3d9/war3/shadow/war3_shadow_backend_native_d3d9.cpp`
   - 新增低频日志：
     - `DXVK War3Shadow: semantic scene skipped empty frame ...`
     - `DXVK War3Shadow: semantic scene submitted draws ...`
     - `DXVK War3Shadow: semantic scene bootstrap catchup success ...`
     - `[NativeShadow] ...`

#### 9.10.37.2 Validation completed this round

1. `ninja -C build32`
   - 多次通过
2. isolated-desktop control-plane runtime validation
   - 手动 launch + `wait_for_game_ready` + `wait_for_hot_shadow_frame`
   - 已首次实证看到：
     - `DXVK War3Hook: Installing native renderer hook takeover after semantic warmup reason=runtime-bridge-warm ...`
     - `DXVK War3: [INFO] [War3Hook] Hooked CWorldFrameWar3::RenderScene @ ...`
   - 这说明：
     - native takeover 安装不再只是“代码上有路径”
     - 而是运行时真的被触发了
3. 新 runtime diagnostics 结论
   - render-thread 的 semantic scene submit 侧当前仍打印：
     - `DXVK War3Shadow: semantic scene skipped empty frame unitsOnly=1 frameSerial=882 drawCount=0 ...`
   - 而同一窗口下 control-plane/hot-frame 侧已经能看到：
     - `semanticCoreSubmittedDrawCount=119`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `runtimeChainWarm=true`
   - 这说明“semantic 数据不存在”已经不是 blocker
   - 真 blocker 变成了：
     - render-thread 当前拿到的 published submission frame 仍然是空帧

#### 9.10.37.3 What is now actually true

1. native takeover 安装这一层已经前推到真实运行时
   - 不再只是 stub / config / docs 层面
2. `RenderScene` hook 也已经实装并出现安装日志
3. 但当前 object-shadow 仍然没有进入 native execute / scene submit 真正出图
   - `nativeD3D9BackendExecutedDrawCount` 仍为 `0`
   - `semanticSceneSubmitted*` 仍为 `0`
4. 当前最准确的工程 blocker 已经收敛成：
   - render-thread 的 `War3TryPopulateSemanticShadowScene(...)`
     - 看到的还是空 `snapshotFrameShared()`
   - 控制面后续能把 semantic frame 热起来
   - 但 render-thread 当前没有及时消费到这张非空 frame
5. 因此下一步主线应继续集中在：
   - semantic frame publish / render-thread consume 时序
   - 而不是再怀疑 native backend 资源缓存 / execute API 是否存在

### 9.10.38 Phase 5B runtime update: takeover-only native execute now passes in the current DXVK host, and the blocker has moved to late-inject/native-only independence plus perf closeout (2026-04-21 23:06:12 +08:00)

#### 9.10.38.1 Code changes landed this round

1. render-thread consume fallback
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.h`
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
   - `src/d3d9/war3/shadow/war3_shadow_native_runtime.cpp`
   - `src/d3d9/d3d9_device.cpp`
   - `ShadowValidationRuntime` 现在保留 `lastRenderableFrame`
   - 当 latest semantic submission 暂时为空时：
     - DXVK semantic scene submit
     - native backend prepare
     - 都会回退到上一张非空 submission frame
2. native takeover gate repair
   - `src/d3d9/war3/core/war3_internal_test_config.h`
     - `kNativeRendererHostExecuteValidationEnabled=false`
   - `src/d3d9/war3/native/war3_native_renderer.cpp`
     - 低频 `INFO` 日志已补：
       - `RenderScene entry`
       - `RenderScene shadow gate`
     - `DriveNativeShadowBackend()` 已不再被
       - `shadowModeOrStage21ListBEntryIndex != -1`
       - 绑死
     - `CWorld_ToggleGroup1ShadowPass(...)` 仍保留为原生兼容辅助，但不再是 object shadow execute 的硬前提
3. control-plane / AutoTest acceptance alignment
   - `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `AutoTest/war3_autotest_mcp.py`
   - `wait_for_hot_shadow_frame` 现改为两段式：
     - 先等 semantic hot frame 成立
     - 再单独轮询 `get_shadow_runtime_summary`
       直到 `nativeD3D9BackendExecutedDrawCount >= threshold`
   - 这样不会再因为 takeover-only execute 稍晚于 semantic hot frame 而把已成功出图误判成失败

#### 9.10.38.2 Validation completed this round

1. `ninja -C build32`
   - 多次通过
2. takeover-only targeted validation
   - 已先验证：
     - 关闭 host execute validation 后
     - `RenderScene` hook 仍真实进入
     - 旧 gate 里 `shadowModeOrStage21ListBEntryIndex==-1`
       确实会把 native execute 卡死
   - 修正 gate 后再次验证：
     - `model_runtime_probe`
       - `nativeD3D9BackendExecutedDrawCount=119`
       - `nativeD3D9BackendSubmittedDrawCount=119`
       - `semanticSceneSubmitted=119`
       - `objectFallbackDrawCount=0`
3. 三套命名场景 takeover-only 回归
   - `model_runtime_probe`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_23_04_53.html`
     - `avgFps=1.574`
     - `nativeD3D9BackendExecutedDrawCount=119`
     - `semanticSceneSubmitted=119`
   - `low_pressure_static_reuse`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_23_05_21.html`
     - `avgFps=1.477`
     - `nativeD3D9BackendExecutedDrawCount=122`
     - `semanticSceneSubmitted=122`
   - `dynamic_shadow_pressure`
     - report:
       - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_23_05_50.html`
     - `avgFps=1.577`
     - `nativeD3D9BackendExecutedDrawCount=119`
     - `semanticSceneSubmitted=119`
4. runtime proof captured this round
   - `DXVK War3: [INFO] [War3Hook] Hooked CWorldFrameWar3::RenderScene @ ...`
   - `DXVK War3: [INFO] [War3Native] RenderScene entry ... shadowStageEntry=0 shadowModeOrStage21=-1 ...`
   - `DXVK War3: [INFO] [War3Native] RenderScene shadow gate ... prepared=1`

#### 9.10.38.3 What is now actually true

1. 当前 DXVK 宿主内，native takeover-only execute 已经跑通
   - 不再依赖 host-side execute validation
2. 当前阶段性完成结论应更新为：
   - semantic-only object shadow
   - `ShadowRendererCore`
   - native backend execute
   - 在 current host 内都已经形成闭环
3. 当前主 blocker 已不再是：
   - `executedDrawCount == 0`
   - 或 `snapshotFrameShared()` 空帧
4. 当前真正剩余的 blocker 已切换为：
   - late-inject / native-only 独立宿主收口
   - 去掉对当前 DXVK host/proxy 壳的运行前提
   - 后续性能收口
5. 当前仍需保留的已知问题：
   - AutoTest 截图尺寸仍为 `1902x963`，尚未回到 `2560x1440`
   - 当前 FPS 仍很低，性能优化尚未开始收口

### 9.10.39 Runtime recovery update: default path no longer uses experimental full-scene native takeover; safe DXVK-host native shadow execute is restored as the current usable path (2026-04-22 00:32:00 +08:00)

#### 9.10.39.1 Why this round was needed

1. 最新用户实机已确认：
   - 打开默认 native takeover 后
   - 游戏内单位/建筑出现大面积白模、纯白贴图、主场景结构损坏
2. 代码事实也已经对齐：
   - `kNativeRendererHookTakeoverEnabled=true`
   - 当前会直接 Hook `CWorldFrameWar3::RenderScene`
   - 并让 `Native_CWorld_RenderScene(...)` 重放完整 RenderScene 主链
3. 因此当前被证实的问题不是“阴影单独坏了”
   - 而是 experimental full-scene takeover 仍会破坏主场景材质/贴图 correctness

#### 9.10.39.2 Code changes landed this round

1. `src/d3d9/war3/core/war3_internal_test_config.h`
   - `kNativeRendererHookTakeoverEnabled=false`
   - `kNativeRendererHostExecuteValidationEnabled=true`
2. 运行策略随之切回：
   - 主场景默认继续由原版 / 当前 DXVK 宿主正常渲染
   - object shadow 继续走：
     - semantic manifest / `ShadowRendererCore`
     - `DxvkValidationBackend`
     - native D3D9 backend host-side execute
3. 当前 native renderer full takeover 的定位已更新为：
   - 非默认实验路径
   - 仅用于后续 native-only / late-inject correctness 单独收口

#### 9.10.39.3 Validation completed this round

1. `ninja -C build32`
   - 通过
2. `model_runtime_probe`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_00_29_23.html`
   - screenshot:
     - `AutoTest/artifacts/screenshots/war3_20260422_002925.png`
   - 结果：
     - 主场景白模已消失，主模型贴图恢复
     - `nativeD3D9BackendExecutedDrawCount=117`
     - `semanticCoreSubmittedDrawCount=117`
     - `objectFallbackDrawCount=0`
3. `low_pressure_static_reuse`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_00_30_15.html`
   - screenshot:
     - `AutoTest/artifacts/screenshots/war3_20260422_003016.png`
   - 结果：
     - `ok=true`
     - `nativeD3D9BackendExecutedDrawCount=117`
     - `objectFallbackDrawCount=0`
4. `dynamic_shadow_pressure`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_00_30_43.html`
   - screenshot:
     - `AutoTest/artifacts/screenshots/war3_20260422_003043.png`
   - 结果：
     - `ok=true`
     - `nativeD3D9BackendExecutedDrawCount=117`
     - `objectFallbackDrawCount=0`

#### 9.10.39.4 What is now actually true

1. 当前仓库默认路径已经不再是 full-scene native takeover
2. 当前“可用态”应表述为：
   - 主画面不再被 takeover 打坏
   - 最新 semantic object shadow 链仍然在当前 DXVK 宿主内执行
   - native backend execute 仍然成立
3. 当前仍未完成的工作不是“先把画面救回来”
   - 这一层已经完成
   - 当前主 blocker 重新收敛为：
     - native-only / late-inject 独立宿主 correctness
     - full-scene takeover 正确性单独收口
     - 后续性能优化

#### 9.10.40 2026-04-22 01:00:48.655 +08:00 Runtime update: semantic 动态阴影已从“只有 execute 计数”推进到“receiver 中实际可见”

##### 9.10.40.1 What was still blocked before this round

1. safe DXVK host 路线虽然已经稳定满足：
   - `semanticCoreSubmittedDrawCount > 0`
   - `semanticSceneSubmitted > 0`
   - `nativeD3D9BackendExecutedDrawCount > 0`
2. 但用户实机与本地 `ShadowFactor` 观察仍显示：
   - dynamic shadow receiver 里大部分区域几乎发白
   - 说明 blocker 不再是“有没有 submit / execute”
   - 而是 semantic caster 在 shadow-map 可见性链里仍然被错误吃掉

##### 9.10.40.2 Code changes landed this round

1. `src/d3d9/d3d9_device.cpp`
   - `War3ApplySemanticBoundsFromMatrix(...)` 不再只吃矩阵平移
   - 现在会把 `candidate.localBoundsCenter` 真实乘进 semantic basis matrix
   - 让 semantic geometry 的 world-space bounds center 不再默认退化成“只看 root translation”
2. `src/d3d9/d3d9_war3_shadow.cpp`
   - `intersectsCascade(...)` 新增单位级联剔除临时豁免
   - 当前对 `ObjectKind::Unit` 先不做 cascade cull
   - 目标是先把新线路动态阴影恢复为稳定可见，再回头细收 bounds contract
3. `src/d3d9/war3/core/war3_internal_test_config.h`
   - 新增并默认启用：
     - `kShadowCascadeCullDisableForUnits = true`
   - 这条开关明确标注为当前 semantic 动态阴影收口期的 correctness-first 临时策略

##### 9.10.40.3 Validation completed this round

1. `ninja -C build32`
   - 通过
2. `dynamic_shadow_pressure` with `DXVK_WAR3_SHADOW_DEBUG=2`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_00_55_12.html`
   - screenshot:
     - `AutoTest/artifacts/screenshots/war3_20260422_005514.png`
   - 关键结果：
     - `semanticCoreSubmittedDrawCount=117`
     - `semanticSceneSubmitted=117`
     - `nativeD3D9BackendExecutedDrawCount=117`
     - `objectFallbackDrawCount=0`
   - `ShadowFactor` 对比：
     - pre-fix `war3_20260422_004656.png`
     - post-fix `war3_20260422_005514.png`
     - 中央场景裁剪区域暗像素占比 `11.803% -> 15.493%`
     - very-dark 占比 `10.427% -> 13.524%`
   - 结论：
     - 新线路 dynamic caster 不再只是“提交成功”
     - 已经更明显地进入 receiver / shadow-map 可见结果
3. `dynamic_shadow_pressure` normal mode
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_00_56_30.html`
   - screenshot:
     - `AutoTest/artifacts/screenshots/war3_20260422_005631.png`
   - 关键结果保持：
     - `semanticSceneSubmitted=117`
     - `nativeD3D9BackendExecutedDrawCount=117`
     - `objectFallbackDrawCount=0`
   - 主场景仍保持：
     - 非白模
     - 非 full-scene native takeover
4. `model_runtime_probe`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_00_58_00.html`
   - screenshot:
     - `AutoTest/artifacts/screenshots/war3_20260422_005801.png`
   - 关键结果保持：
     - `semanticCoreSubmittedDrawCount=117`
     - `semanticSceneSubmitted=117`
     - `nativeD3D9BackendExecutedDrawCount=117`
     - `objectFallbackDrawCount=0`

##### 9.10.40.4 What is now actually true

1. 当前 safe DXVK host 路线上的新 semantic 动态阴影，已经不再只是“execute counter > 0”
2. 当前更准确的说法是：
   - 新线路 dynamic shadow 已经在 receiver / ShadowFactor 中实质可见
   - 主画面仍保持 safe host 路线，不会回到白模 takeover
3. 当前仍未完成：
   - 当前单位级联 no-cull 仍是 correctness-first 临时策略
   - `avgFps` 仍只有大约 `0.57`
   - screenshot 仍是 `1902x963`，未恢复到 `2560x1440`
   - native-only / late-inject 独立宿主仍未完成

### 9.10.41 Upstream semantic data round: rolled back the over-strict runtime-model validator, kept the geoset-owner recovery, and started shrinking real `no-pose / no-runtime-group-palette` misses (2026-04-22 02:38:19.914 +08:00)

#### 9.10.41.1 What was actually blocking this round

1. 当前不是“拿不到任何模型/姿态数据”。
2. 真问题是两类：
   - 一类是上游噪声：`sceneNode` / blocker-like record 混进 semantic consumer，污染 `no-pose` 统计。
   - 一类是真正的链路缺口：`phase=no-descendant-runtime` 下，root pose palette 只有 `1~3`，但 geoset group 需要 `21/25/79` 级别的 palette。
3. 上一轮试验性“更严格 runtime-model 判定 + pose identity backfill”已经证明不是正确方向：
   - 会把 hot-frame 拉回退化态。
   - 因此本轮先完整回退到安全基线，再继续做更窄的补链。

#### 9.10.41.2 Code changes landed this round

1. 完整回退了 `war3_shadow_renderer_core.cpp` 里最后一处 over-strict `LooksLikeRuntimeModelPtr(...)`，恢复到与其余模块一致的宽松判定。
2. 保留并继续使用上一轮已证明安全的 geoset-owner recovery：
   - `ShadowModelResourceCache::findRuntimeModelOwner(...)`
   - `VisibleRenderableRecord` 按 `runtimeGeosetPtr / runtimeGeosetDataPtr / geosetIndex` 回补 `runtimeModelPtr + modelResourcePtr + modelKey`
   - `ShadowRendererCore` 在 pose/runtime-root 搜索时优先纳入这条 owner 线索
3. 继续沿 `no-descendant-runtime` 真 blocker 做最小修正：
   - `TryResolveChildRuntimeModelByModelResource(...)` 现在会先把 `targetModelResourcePtr` 与 child runtime 的 `OwnedModelDataHandle` 都归一化成 direct model resource 再比对，避免把 handle/resource 混比。
4. 对 semantic consumer 增加了更窄的上游噪声隔离：
   - `VisibleRenderableRecord` / `ModelInstanceRegistry` 会清理明显错误的 self-pointer runtime 候选（`runtimeModelPtr == sceneNode/worldObjectEntry/sprite/renderablePart/meshData`）。
   - `ShadowRendererCore` 在 `no-pose` 统计前会把 LOS/path-blocker FourCC 噪声从计数里剔掉，不再让这类对象继续污染 `no-pose` 指标。

#### 9.10.41.3 Validation completed this round

1. `ninja -C build32`
   - 多次通过
2. 安全基线重建：
   - artifact: `AutoTest/artifacts/war3_upstream_quick6.json`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_02_23_49.html`
   - 关键结果：
     - `recordsWithModelResource=818`
     - `recordsWithRuntimeModel=888`
     - `semanticCoreResolved=376`
     - `semanticCoreSkippedNoPose=82`
     - `semanticCoreSkippedNoRuntimeGroupPalette=442`
3. 本轮改动后最好的 quick 结果：
   - artifact: `AutoTest/artifacts/war3_upstream_quick9.json`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_02_37_45.html`
   - 关键结果：
     - `recordsWithModelResource=816`
     - `recordsWithRuntimeModel=886`
     - `semanticCoreResolved=378`
     - `semanticCoreSkippedNoPose=71`
     - `semanticCoreSkippedNoPoseLookupMiss=59`
     - `semanticCoreSkippedNoRuntimeGroupPalette=441`
     - `nativeD3D9BackendExecutedDrawCount=378`
4. live 复核：
   - artifacts:
     - `AutoTest/artifacts/war3_upstream_live3.json`
     - `AutoTest/artifacts/war3_upstream_live7.json`
   - 结论：
     - `semanticCoreSkippedNoPose` 从基线 `82` 稳定降到 `71`
     - `semanticCoreSkippedNoRuntimeGroupPalette` 在 live run 里仍大致停在 `442`
     - 说明“噪声型 no-pose”开始被压下，但 child-runtime palette 缺口还没有被真正收掉

#### 9.10.41.4 What is now actually true

1. 我们已经确认：
   - geoset-owner recovery 是安全的
   - over-strict runtime-model validator 不是正确方向
   - `no-pose` 里确实混着一部分 upstream noise，而不是纯粹“真没有 pose”
2. 当前最新真实状态是：
   - `semanticCoreResolved` 已从 `376` 拉到 `378`
   - `semanticCoreSkippedNoPose` 已从 `82` 压到 `71`
   - `semanticCoreSkippedNoRuntimeGroupPalette` 只从 `442` 压到 `441`
3. 因此当前 primary blocker 已进一步收敛为：
   - `phase=no-descendant-runtime`
   - 典型样本是 `poseCount=1/2/3`，但 `groupCount/maxSlot` 需要 `21/25/79`
   - 这说明 child runtime 的 palette capture / linkage 仍未真正对上 geoset 需求
4. 下一轮应继续直接沿这条线推进：
   - 优先检查 child runtime palette capture / bind / publish 链
   - 而不是再回头扩大 runtime-model 判定范围

### 9.10.42 Upstream semantic data round: proved the remaining blocker is missing descendant pose publication, not stale consumer lookup (2026-04-22 03:04:35.581 +08:00)

#### 9.10.42.1 What we changed this round

1. consumer 侧先把两处无失效静态缓存去掉：
   - `CollectRuntimeModelTree(...)`
   - `TryResolveChildRuntimeModelByModelResource(...)`
   避免把早期 no-hit 或半热 tree 结果整进程缓存下来。
2. descendant runtime -> model resource 解析改成优先信任已发布绑定：
   - `ShadowModelResourceCache::findRuntimeModelResource(...)`
   - `ModelRegistry::findByRuntimeModel(...)`
   - 最后才回退到 live `OwnedModelDataHandle`
3. `VisibleRenderableRecord` 进入 geoset binding 前新增 runtimeModel 清洗：
   - 只要 `runtimeModelPtr` 明显等于 `sceneNode/worldObjectEntry/sprite/renderablePart/meshData`
   - 直接清空并重新走 metadata recovery
4. `ShadowRendererCore` 新增两条 runtime-side 兜底：
   - `no-descendant-runtime detail` 日志，直接输出 `descRuntimeCount/descPoseHitCount/bestDescPoseCount`
   - 当 snapshot pose 缺失时，对 descendant runtime 做 on-demand live matrix-palette read
5. `war3_model_hook.cpp` 的 palette capture 再往前推一轮：
   - `Hook_RuntimePoseUpdate(...)`
   - `Hook_RuntimeMatrixRangeCopy(...)`
   - `Hook_RuntimeMatrixFlush(...)`
   现在都会调用 `RecordRuntimePaletteTree(runtimeModel)`，不再只记录当前 runtime。

#### 9.10.42.2 Validation completed this round

1. `ninja -C build32`
   - 多次通过
2. fresh live probes：
   - artifacts:
     - `AutoTest/artifacts/war3_upstream_live10.json`
     - `AutoTest/artifacts/war3_upstream_live12.json`
     - `AutoTest/artifacts/war3_upstream_live13.json`
   - 稳定结果：
     - `semanticCoreResolved=376~378`
     - `semanticCoreSkippedNoPose=71`
     - `semanticCoreSkippedNoRuntimeGroupPalette=442`
     - `nativeD3D9BackendExecutedDrawCount=376~378`
3. 当前 authoritative quick 结果：
   - artifact:
     - `AutoTest/artifacts/war3_upstream_quick14.json`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_22_03_04_06.html`
   - 关键结果：
     - `recordsWithModelResource=816`
     - `recordsWithRuntimeModel=886`
     - `semanticCoreResolved=376`
     - `semanticCoreSkippedNoPose=71`
     - `semanticCoreSkippedNoRuntimeGroupPalette=442`
     - `nativeD3D9BackendExecutedDrawCount=376`

#### 9.10.42.3 The new hard evidence

1. `no-descendant-runtime detail` 日志已经把当前 blocker 压成了可执行事实：
   - 典型样本 1：
     - `runtime=2f0b5f64 model=2d1a07e8 descRuntimeCount=1 descPoseHitCount=0`
   - 典型样本 2：
     - `runtime=1bb5af40 model=2d1a05c0 descRuntimeCount=2 descPoseHitCount=0`
2. 这说明：
   - consumer 侧已经能看到 descendant runtime tree
   - 但 descendant runtime 自己没有任何 snapshot pose
   - 当前轮新增的 on-demand live read 也没有把 `descPoseHitCount` 从 `0` 拉起来
3. 因此 stale cache、runtimeModel->modelResource live handle 解析、以及“只差 consumer lookup”都已经被排除为 primary blocker。

#### 9.10.42.4 What is now actually true

1. 当前问题已经不能再笼统地说成“上游数据不对”。
2. 更准确的表述是：
   - remaining miss family 的 descendant runtime 根本没有进入可用 pose contract
   - live 侧也读不到 descendant runtime 的 matrix palette
3. 当前主线仍然保持可用：
   - safe DXVK host 下动态阴影仍可见
   - object fallback 仍是 `0`
   - native host execute 仍在工作
4. 当前下一步已明确收敛为：
   - 找到并补新的 descendant pose capture / publish 来源
   - 而不是继续扩大 visible/consumer 猜测或继续加旧式 fallback

### 9.10.43 Upstream semantic data round: expanded wrapper-level pose coverage and descendant runtime owner records, but the primary blocker is still missing descendant pose publication (2026-04-22 03:32:10.241 +08:00)

#### 9.10.43.1 What we changed this round

1. `war3_model_hook.cpp` 先补齐了 IDA 已确认但此前未挂的两条 sprite pose/update 路径：
   - `0x6F1820C0`
   - `0x6F1825E0`
   当前四条 wrapper-level update path：
   - `0x6F1820C0 / 0x6F182300 / 0x6F1825E0 / 0x6F1826C0`
   都会在返回时走同一套 `RecordSpriteFramePoseFromSprite(...)`。
2. wrapper-level 返回点不再只记 `sprite frame transform`；
   现在会顺手对 `sprite + 0x20` 取到的 runtime root 调 `RecordRuntimePaletteTree(...)`。
3. `RecordRuntimePaletteTree(...)` 前面新增了一层 tree-level binding pass：
   - 先遍历整棵 runtime tree
   - 对每个 runtimeModel 都调用 `ShadowModelResourceCache::noteRuntimeModelBinding(...)`
   - 让 descendant runtime 即使当前没有 matrix palette，也能先进入 `runtimeModelOwner` 查找面
4. `ShadowRendererCore` 新增一条更窄的 owner-root rescue：
   - 如果 `CollectRenderableRuntimeModelRoots(...)` 已经通过 `runtimeGeosetPtr/runtimeGeosetDataPtr/geosetIndex` 算出了更可信的 runtime root
   - 先直接尝试用这个 root 做 canonical group palette build
   - 只有失败后才继续走“按 modelResource 再找 child runtime”那条旧路径

#### 9.10.43.2 Validation completed this round

1. `ninja -C build32`
   - 多次通过
2. fresh quick/live artifacts：
   - `AutoTest/artifacts/war3_upstream_quick15.json`
   - `AutoTest/artifacts/war3_upstream_live14.json`
   - `AutoTest/artifacts/war3_upstream_quick16.json`
   - `AutoTest/artifacts/war3_upstream_live15.json`
   - `AutoTest/artifacts/war3_upstream_quick17.json`
   - `AutoTest/artifacts/war3_upstream_live16.json`
3. 当前本轮最有价值的稳定信号不是 quick pass/fail，而是 counter 变化：
   - `shadowRuntimeModelCount`
     - 从上一轮 authoritative `218` 抬到 `309~310`
   - `semanticCoreResolved`
     - `376~379`
   - `semanticCoreSkippedNoRuntimeGroupPalette`
     - live 最低到了 `437~438`
   - `nativeD3D9BackendExecutedDrawCount`
     - `376~379`
4. 本轮 quick 还出现了新的 hot-frame wait 抖动：
   - `war3_upstream_quick16/17`
   - `hotShadow.ok=false`
   - `error=wait_until timeout`
   但 hot summary 本身仍然能返回有效 semantic counters。

#### 9.10.43.3 What this round actually proved

1. 当前 remaining blocker 不是“descendant runtime 根本没进 runtimeModel owner 面”：
   - tree-level binding pass 之后
   - `shadowRuntimeModelCount` 已明显上升到 `309~310`
2. 但 primary blocker 依然不是“owner lineage 还没接上”：
   - `war3_upstream_live15.json`
   - `war3_upstream_live16.json`
   里的 `no-descendant-runtime detail` 仍然稳定是：
   - `descPoseHitCount=0`
   - `matchingModelDescCount=0`
   - `bestDescPoseCount=0`
   - `bestMatchingModelPoseCount=0`
3. 这说明：
   - wrapper-level pose/update coverage 扩到四条路径后
   - descendant runtime 的 owner/runtimeModel 记录已经更多了
   - 但 descendant pose 仍没有进入可消费的 snapshot/live contract
4. 换句话说：
   - “先把 descendant runtime 本体记进 cache”
     这一步已经开始起作用
   - 但真正卡住 correctness 的，仍然是 descendant pose publication 本身
     而不是只差一个 lineage key

#### 9.10.43.4 Current next step

1. 下一轮不该再继续重复做的事：
   - 不要再只扩 wrapper-level pose hook 覆盖
   - 不要再继续假设“多记一些 runtime owner 就会自动拿到 descendant pose”
2. 下一轮应继续直接推进的事：
   - 查新的 descendant pose publish 源
   - 优先检查：
     - `0x6F12EC90 -> sub_6F77C280(...)`
     - 以及 child/attachment runtime 在 `post 0x6F12F7E0` 之后是否根本不把最终姿态留在 `CModel + 0x5C/+0x60`
3. 当前更准确的 blocker 表述已经更新为：
   - descendant runtime 的 owner lineage 开始补齐
   - 但 descendant pose publication 仍未进入 stable contract
   - 这才是 `no-runtime-group-palette` 还卡着的根因

### 9.10.44 Upstream semantic data round: post-0x6F12EC90 still does not expose descendant palette, and IDA now supports that this chain is controller/local-point propagation rather than final 3x4 palette publication (2026-04-22 03:39:28.096 +08:00)

#### 9.10.44.1 What we changed this round

1. `war3_model_hook.cpp` 新增了更贴近 child recursion publish 时机的 hook：
   - `post 0x6F12EC90`
   - 也就是 current docs 里的 `CModelComplex__RecurseChildRuntimeTree`
2. 这条 hook 当前只做最小被动采样：
   - 对当前 `runtimeModel` 直接调用 `RecordRuntimeMatrixPalette(runtimeModel)`
   - 不再等到 `post 0x6F12F7E0` 或 wrapper-level path 才尝试读取 descendant runtime 的 `+0x5C/+0x60`
3. 同时用 IDA 复核了：
   - `0x6F77C280`
   - `0x6F77C1D0`
   - `0x6F77CDD0`
   这一组 `sub_6F12EC90` 的核心 callee

#### 9.10.44.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. fresh live artifact：
   - `AutoTest/artifacts/war3_upstream_live17.json`
3. live17 关键结果：
   - `shadowRuntimeModelCount=310`
   - `semanticCoreResolved=378`
   - `semanticCoreSkippedNoPose=88`
   - `semanticCoreSkippedNoRuntimeGroupPalette=440`
   - `nativeD3D9BackendExecutedDrawCount=378`
4. 更关键的是 `no-descendant-runtime detail` 聚合结果仍然完全没动：
   - `descPoseHitPositive = 0`
   - `matchingModelPositive = 0`
   - `max descPoseHitCount = 0`
   - `max matchingModelDescCount = 0`
   - `max bestDescPoseCount = 0`
   - `max bestMatchingModelPoseCount = 0`

#### 9.10.44.3 What IDA now adds to the picture

1. `0x6F12EC90` 本身做的是：
   - `controller = *(runtimeModel + 0x98? / +0x152 in decompile view)`
   - `sub_6F77C280(controller, modelData + 92)`
   - 再沿 child link 递归
2. `0x6F77C280` 的实质结构是：
   - `sub_6F77BFE0(...)`
   - 遍历 `v3 + 128 / +132` 那组 220B records
   - 对每个 record 调 `0x6F77CDD0`
   - 最后 `sub_6F77CF40(a1)`
3. `0x6F77CDD0` 的尾部不是 palette copy：
   - 它走 `sub_6F789EB0 / sub_6F789C60`
   - 最后回调 `(*(v3 + 204))(v12 + 22, &v9, *(v3 + 208))`
4. 当前更合理的解释已经不是“这里在给 child runtime 发布最终 3x4 palette”，而是：
   - 这条链更像 controller/local-point/attachment output 传播
   - 而不是 `CModel_CopyResolvedPoseMatricesToOutputPalette(0x6F12FDC0)` 那种最终 palette publish

#### 9.10.44.4 What this round actually proved

1. 现在已经不能再把希望寄托在“只要把时机推近到 `post 0x6F12EC90`，descendant palette 就会出现”。
2. 因为：
   - runtime 侧 `post 0x6F12EC90` probe 也没有抓到任何 descendant `+0x5C/+0x60` palette
   - IDA 侧又支持这条链更像 controller/local-point propagation
3. 当前更强的工作结论已经变成：
   - child/attachment runtime 很可能根本不把最终姿态稳定留在 `CModel + 0x5C/+0x60`
   - 至少当前 remaining miss family 里，看不到它们通过这块 contract 暴露给 semantic consumer

#### 9.10.44.5 Current next step

1. 下一轮应直接收敛到两个方向之一：
   - 查 child/attachment runtime 的真正 pose publish contract
   - 或明确把这批对象从“canonical runtime group palette 路线”里拆出来，改走单独 contract
2. 当前最值得优先查的点：
   - `0x6F77BFE0 / 0x6F77CF40 / 0x6F789C60`
   - 它们到底在产出什么 controller/output block
   - 这些 block 是否能反推出 child runtime 的最终 transform / local matrix

### 9.10.45 Upstream semantic data round: controller-output probe proves local-point writes are alive, while primary/shared preset writes remain zero in `model_runtime_probe` (2026-04-22 03:58:32.102 +08:00)

#### 9.10.45.1 What we changed this round

1. `war3_model_hook.cpp`
   - 新增三条只读 output-write probe：
     - `0x6F77DA20`
     - `0x6F77DAA0`
     - `0x6F77DF10`
   - 分别对：
     - local-point output
     - shared preset output
     - primary preset output
     做最小被动采样
2. `war3_model_hook.h`
   - 新增 `RuntimeOverrideOutputProbeSummary`
   - 新增 `QueryRuntimeOverrideOutputProbeSummary()`
3. `war3_shadow_runtime_bridge.*`
   - 把 controller-output probe 摘要并入现有 `ShadowRuntimeBridgeSummary`
4. `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 现在会直接返回：
     - `overridePrimaryPresetWriteCount`
     - `overrideSharedPresetWriteCount`
     - `overrideLocalPointWriteCount`
     - `overrideLocalPointNonZeroWriteCount`
     - `overrideMaxLocalPointSlotIndex`
     - `overrideLastRuntimeModelPtr`
     - `overrideLastLocalPointSlotIndex`
     - `overrideLastLocalPointX/Y/Z`

#### 9.10.45.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. fresh artifacts：
   - `AutoTest/artifacts/war3_upstream_live18.json`
   - `AutoTest/artifacts/war3_upstream_quick18.json`
3. `war3_upstream_live18.json`
   - `hotShadow.ok=false`
   - `error=wait_until timeout`
   - 但 hot summary 本体已返回有效新 probe 字段：
     - `overrideOutputSampleFrame=927`
     - `overrideOutputLastActiveFrame=927`
     - `overridePrimaryPresetWriteCount=0`
     - `overrideSharedPresetWriteCount=0`
     - `overrideLocalPointWriteCount=42543`
     - `overrideLocalPointNonZeroWriteCount=39165`
     - `overrideMaxLocalPointSlotIndex=10`
     - `overrideLastRuntimeModelPtr=290791012`
     - `overrideLastLocalPointSlotIndex=0`
     - `overrideLastLocalPointX/Y/Z=-1.7233 / 0.8925 / 0.0`
     - 同帧仍是：
       - `semanticCoreResolved=393`
       - `semanticCoreSkippedNoRuntimeGroupPalette=420`
       - `nativeD3D9BackendExecutedDrawCount=393`
       - `semanticCoreFrameFresh=false`
4. `war3_upstream_quick18.json`
   - 放宽 `require_semantic_frame_fresh=false` 后：
     - `hotShadow.ok=true`
   - 关键结果：
     - `overrideOutputSampleFrame=886`
     - `overridePrimaryPresetWriteCount=0`
     - `overrideSharedPresetWriteCount=0`
     - `overrideLocalPointWriteCount=4805`
     - `overrideLocalPointNonZeroWriteCount=4433`
     - `overrideMaxLocalPointSlotIndex=10`
     - 同时：
       - `semanticCoreResolved=378`
       - `semanticCoreSkippedNoRuntimeGroupPalette=442`
       - `nativeD3D9BackendExecutedDrawCount=378`
       - `semanticCoreFrameFresh=false`

#### 9.10.45.3 What this round actually proved

1. 当前 blocker 已经不能再表述成：
   - “controller/output block 可能根本没有在热帧里被写”
2. 因为 fresh live/quick 现在已经明确显示：
   - local-point output 写回是活的
   - 而且是大量、非零、连续活跃的
3. 同一轮里 primary/shared preset probe 仍然是：
   - `overridePrimaryPresetWriteCount=0`
   - `overrideSharedPresetWriteCount=0`
4. 因此当前更强的新结论变成：
   - child/attachment 这批 remaining miss family 至少有一部分已经通过 `node_type=7 / local-point output` 暴露出了上游 contract
   - 但 semantic shadow consumer 目前完全没有消费这批 output
   - 而当前 `model_runtime_probe` 场景里，看不到 primary/shared preset 输出在参与这批 miss family
5. 换句话说：
   - 下一轮不该再继续扩 `CModel + 0x5C/+0x60`
   - 也不该优先押注 primary/shared preset output
   - 真正该追的是：
     - `local-point output slot`
     - `child link tag / outputSlotIndex`
     - 它们和 missing descendant runtime/geoset 的映射关系

#### 9.10.45.4 Current next step

1. 下一轮应直接收敛到：
   - `sub_6F77DB20 / sub_6F77DA20`
   - `runtime + 0xB4`
   - `child link + 0x0C (attach point / child tag)`
2. 目标不是再证明 output block 存在，而是：
   - 把 `local-point output slot -> child/attachment runtime identity` 对上
   - 判断这批对象能否直接转成 rigid transform contract
3. 当前最不值得继续做的事：
   - 继续扩大 `+0x60` palette hook 覆盖
   - 或继续默认把 primary/shared preset 路线当成当前 main missing route

### 9.10.46 Upstream semantic data round: local-point mapping probe confirms the current override write context exposes neither direct child links nor a nearby owner/runtime root with child links (2026-04-22 04:21:07.715 +08:00)

#### 9.10.46.1 Code changes landed this round

1. `war3_model_hook.cpp/.h`
   - 在既有 `local-point -> child link` probe 基础上，继续补了三类最小映射诊断：
     - `sourceRecordIndex -> child link +0x0C`
     - `当前 local-point runtime 是否直接带 child links`
     - `override write context 前 0x40 字节里是否还能找到带 child links 的 owner/root runtime`
2. `war3_shadow_runtime_bridge.*`
   - 把上述新 probe 字段继续接进 `ShadowRuntimeBridgeSummary`
3. `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 现在直接返回：
     - `overrideLocalPointObservedChildLinkWriteCount`
     - `overrideLocalPointMatchedChildLinkBySourceRecordWriteCount`
     - `overrideLocalPointContextRuntimeWithChildLinksWriteCount`
     - `overrideLastContextRuntimeWithChildLinksOffset/Ptr/Count/MaxTag`

#### 9.10.46.2 Validation completed this round

1. `ninja -C build32`
   - 通过
2. fresh artifacts：
   - `AutoTest/artifacts/war3_upstream_quick19.json`
   - `AutoTest/artifacts/war3_upstream_quick20.json`
   - `AutoTest/artifacts/war3_upstream_quick21.json`
3. `war3_upstream_quick19.json`
   - `hot.ok=true`
   - `overrideLocalPointWriteCount=4805`
   - `overrideLocalPointNonZeroWriteCount=4433`
   - `overrideLocalPointMatchedChildLinkWriteCount=0`
   - `overrideLocalPointMatchedChildLinkBySourceRecordWriteCount=0`
   - `overrideLastLocalPointSourceRecordIndex=13`
4. `war3_upstream_quick20.json`
   - `hot.ok=true`
   - 新 probe 直接给出：
     - `overrideLocalPointObservedChildLinkWriteCount=0`
     - `overrideMaxObservedChildLinkCount=0`
     - `overrideMaxObservedChildLinkTag=0`
     - `overrideLastObservedChildLinkCount=0`
   - 这说明当前 local-point write 所在的 `runtimeModel` 本身根本没有 direct child links 可供映射
5. `war3_upstream_quick21.json`
   - `hot.ok=true`
   - 继续对 `contextPtr` 前 `0x40` 做 runtime-pointer 扫描后，结果仍是：
     - `overrideLocalPointContextRuntimeWithChildLinksWriteCount=0`
     - `overrideLocalPointContextMatchedChildLinkWriteCount=0`
     - `overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount=0`
     - `overrideLastContextRuntimeWithChildLinksPtr=0`
     - `overrideLastContextRuntimeWithChildLinksOffset=0`
     - `overrideLastContextRuntimeWithChildLinksCount=0`
   - 同轮 semantic 仍然是：
     - `semanticCoreResolved=381`
     - `semanticCoreSkippedNoRuntimeGroupPalette=442`

#### 9.10.46.3 What this round actually proved

1. 当前 blocker 已经不能再表述成：
   - “也许只是 `slotIndex -> tag` 没对上”
   - 或“也许换成 `sourceRecordIndex` 就会命中”
2. 因为 fresh quick19/20/21 现在已经连续证明：
   - local-point output 写回仍然是热的
   - 但当前 write context 的 `runtimeModel` 不带 direct child links
   - 并且在当前 `contextPtr` 的近邻 builder/eval context 窗口里，也找不到任何带 child links 的 owner/root runtime 指针
3. 因此当前更强的新结论变成：
   - remaining child/attachment miss family 的 primary blocker 已经不是 `tag remap`
   - 而是 `override write context -> owner/root runtime` 这段 handoff 根本还没拿到
4. 换句话说：
   - 在找回 owner/root runtime 之前，继续穷举 `slot/tag/sourceRecord` 的对应关系都不会得到 authoritative 结果

#### 9.10.46.4 Current next step

1. 下一轮应直接收敛到：
   - `sub_6F77C280 / sub_6F77C260` 的 eval context 结构
   - `controller -> scratch/output root`
   - 它们如何把当前 local-point write 关联回真正的 complex runtime root
2. 目标不再是继续猜：
   - `slotIndex == tag`
   - `sourceRecordIndex == tag`
3. 而是先找回：
   - `owner/root runtimeModel` 的 authoritative 指针来源
   - 再基于那棵 root 的 child links 判断 child/attachment 能否转 rigid transform contract

### 26. 2026-04-22 attachment rigid contract live-fallback 已验证生效，但 identity 仍然完全缺失

#### 26.1 本轮代码推进

1. `war3_shadow_runtime_contract.cpp`
   - attachment rigid 现在不只在 `captureLiveState()` 里发布；
   - 当已发布 contract 的 attachment 为空，但 live registry 里已经有 attachment record 时，
     `snapshotAttachments()/snapshotAttachmentsShared()/snapshotBundleShared()` 会按当前 manifest 即时合成 live attachment store；
   - 同时补了 contract-build 阶段的 identity repair：
     - `runtimeModel -> PoseRegistry / ModelInstanceRegistry / ShadowObjectRegistry / VisibleRenderableRegistry`
     - `runtimeModel -> ShadowModelResourceCache -> modelResource/modelKey -> 唯一 ShadowObject/manifest 候选`
2. `war3_model_registry.*` / `war3_model_hook.cpp`
   - attachment rigid record 新增 `ownerRuntimeModelPtr`
   - 当前写入策略固定为优先保存 `argBlockRuntime.runtimeModelPtr`，再退到 `arg4BlockRuntime.runtimeModelPtr`，最后退到当前 `runtimeModelPtr`
3. `war3_shadow_renderer_core.cpp`
   - attachment rigid pose lookup 现在优先尝试 `ownerRuntimeModelPtr`，再退回 `rootRuntimeModelPtr`
4. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - control-plane summary 新增：
     - `contractAttachmentRigidCount`
     - `contractAttachmentRigidCountWithAnyIdentity`
     - `contractAttachmentRigidCountWithWorldObjectEntry`
     - `contractAttachmentRigidCountWithSceneNode`
     - `contractAttachmentRigidCountWithUnitPtr`

#### 26.2 本轮验证

1. `ninja -C build32`
   - 连续通过
2. fresh runtime 验证
   - 连续使用 `run_named_scenario(model_runtime_probe)` 做 pipe-first 验证
   - 最新完成时间点：`2026-04-22 12:43:33 +08:00`
3. 当前最关键的新结果已经稳定：
   - `attachmentRigidCount = 3`
   - `contractAttachmentRigidCount = 3`
   - 这说明“attachment rigid 只在 raw registry 里存在、contract 完全吃不到”的问题已经被修掉
4. 但同轮也连续确认：
   - `attachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithSceneNode = 0`
   - `contractAttachmentRigidCountWithUnitPtr = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`
5. control-plane / hot-frame safety gate 这轮保持稳定：
   - `requestedSemanticFrameBuild = false`
   - 没有再出现 CPU storm
   - 失败仍然是安全拒绝：
     - `render.isInGame = false`
     - `wait_until stalled`

#### 26.3 这轮真正证明了什么

1. 现在可以把一个问题正式从 blocker 列表里移掉：
   - “attachment rigid 是不是根本没进 contract”
   - 答案已经变成：不是，contract 现在已经能稳定看到这 3 条 attachment record
2. 当前 remaining primary blocker 也因此变得更硬：
   - downstream join 这条路目前不够
   - 无论是：
     - `rootRuntimeModelPtr`
     - `ownerRuntimeModelPtr`
     - `childRuntimeModelPtr`
     - `Pose/Instance/ShadowObject/Visible registry`
     - `modelResource/modelKey` 唯一候选兜底
   - 都还不能把这 3 条 attachment 补出任何 `worldObjectEntry/sceneNode/unitPtr/jHandle/rawcode`
3. 这说明当前 attachment miss family 的问题，已经不能再定义成：
   - “contract publish 太早”
   - “还是缺一个下游 join 键”
4. 当前更准确的新结论是：
   - local-point producer/controller 链当前没有把 authoritative object identity 一起暴露出来
   - 或者至少没有通过现有 runtime/registry contract 暴露出来

#### 26.4 当前下一步

1. 下一轮主线应切到：
   - 直接从 `local-point output` 的 producer/controller/eval-context 链回收 identity owner
   - 不再继续把希望放在 downstream repair/join
2. 目标应改成查清：
   - 哪个上游对象真正持有这批 child/attachment 的 `sceneNode/worldObjectEntry/unitPtr`
   - 它与当前 `argBlockRuntime/rootRuntime/childRuntime` 之间的 authoritative handoff 是什么
3. 在这条上游 identity contract 拿到之前：
   - 再继续加更多 manifest/registry/modelResource 级别的 join，大概率只会重复得到 `identity=0`

### 27. 2026-04-22 `CSprite_AttachModelToPoint` 已确认是 live 挂接点，但这层 parent sprite 仍然没有 logical identity

#### 27.1 本轮代码推进

1. `war3_shadow_runtime_contract.cpp`
   - attachment rigid repair 新增 `runtimeModel -> sprite -> parentSprite` 补链；
   - 目标是验证：attached-effect hook 已经暖起来的 sprite/runtime 关系，能不能直接把 remaining attachment rigid 补出 identity。
2. `war3_model_hook.cpp/.h`
   - 新增 `0x6F184E50` (`CSprite_AttachModelToPoint`) hook；
   - 该 hook 直接观察 `parentSprite + attachPointIndex + childSprite` 的真实挂接点；
   - 新增 `attachModelToPoint*` summary/counter，用来区分：
     - 这层到底有没有 live 命中
     - 命中后 parent/owner identity 有没有真的解析出来
3. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - control-plane summary 新增：
     - `attachModelToPointBindCount`
     - `attachModelToPointResolvedIdentityCount`
     - `attachModelToPointResolvedUnitCount`
     - `attachModelToPointResolvedHandleCount`
     - `attachModelToPointResolvedRawcodeCount`
     - `lastAttachModelToPointParentSpritePtr`
     - `lastAttachModelToPointChildSpritePtr`
     - `lastAttachModelToPointChildRuntimeModelPtr`

#### 27.2 本轮验证

1. `ninja -C build32`
   - 通过
2. fresh runtime 验证
   - 连续使用 `run_named_scenario(model_runtime_probe)` 做 pipe-first 验证
   - 最新完成时间点：`2026-04-22 14:54:25 +08:00`
3. 先验证 attachment sprite-chain repair
   - 结果仍然是：
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
   - 这说明仅靠现有 runtime/sprite/parentSprite repair，仍然补不回 remaining attachment identity
4. 再验证 `CSprite_AttachModelToPoint`
   - 新结果是：
     - `attachModelToPointBindCount = 18`
     - `attachModelToPointResolvedIdentityCount = 0`
     - `attachModelToPointResolvedUnitCount = 0`
     - `attachModelToPointResolvedHandleCount = 0`
     - `attachModelToPointResolvedRawcodeCount = 0`
     - `lastAttachModelToPointParentSpritePtr != 0`
     - `lastAttachModelToPointChildSpritePtr != 0`
     - `lastAttachModelToPointChildRuntimeModelPtr != 0`
   - 同轮 remaining attachment 仍然是：
     - `attachmentRigidCount = 2`
     - `contractAttachmentRigidCount = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`

#### 27.3 这轮真正证明了什么

1. `0x6F184E50` 不是死点
   - 它在当前 `model_runtime_probe` 下是热的，18 次命中已经足够把“这个函数是不是根本没走到”排除掉
2. 但 `0x6F184E50` 也不是当前 missing family 的 identity owner 点
   - 因为即使到了 `parentSprite -> childSprite` 的真实挂接点，当前仍然拿不到任何：
     - `unitPtr`
     - `jHandle`
     - `rawcode`
   - 也就是：这层看到的 parent sprite 依然是匿名的
3. 这把 blocker 又收紧了一层
   - 当前问题已经不能再定义成：
     - “是不是还没 hook 到真正的挂接点”
   - 现在更准确的结论是：
     - 逻辑身份并不是在 `CSprite_AttachModelToPoint` 这层才第一次出现
     - 真正的 identity owner 还在更上游的 `object/widget -> sprite` 发布层

#### 27.4 当前下一步

1. 下一轮不再继续重复：
   - `parentSprite/runtimeModel` 下游 repair
   - `CSprite_AttachModelToPoint` 同层更多枚举
2. 下一轮主线应继续上抬到：
   - `CreateAttachedEffect`
   - `CWidget_CreateAndAttachEffect`
   - `CWidget_CreateAndAttachEffectInternal`
   - 或其它“逻辑对象 / widget 指针 与目标 sprite 同时共存”的 owner 发布点
3. 目标已经进一步锁死为：
   - 找到一个同帧 join key，把 logical object identity 直接绑定到 parent/root sprite 或 runtimeModel
   - 而不是再指望 attachment contract 自己在更下游把 identity 猜回来

### 28. 2026-04-22 `AttachedEffectInit` 已证实能解析 attachment identity，但 current anonymous attachment rigid 仍未吃到这批 identity

#### 28.1 本轮代码推进

1. `war3_model_hook.cpp/.h`
   - 新增 `0x6F6B9FF0` direct-attach hook；
   - 新增两组分层 counter：
     - `attachedEffectInit*`
     - `attachedEffectDirect*`
   - 目的不再是笼统看 `AttachModelToPoint`，而是把：
     - `0x6F6BB2C0` (`AttachedEffectInit`)
     - `0x6F6B9FF0` (direct attach)
     - `0x6F184E50` (`AttachModelToPoint`)
     三条链在当前场景里拆开签收。
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 新增：
     - `attachedEffectInitBindCount`
     - `attachedEffectInitResolvedIdentityCount`
     - `attachedEffectInitResolvedUnitCount`
     - `attachedEffectInitResolvedHandleCount`
     - `attachedEffectInitResolvedRawcodeCount`
     - `attachedEffectDirectBindCount`
     - `attachedEffectDirectResolvedIdentityCount`
3. `war3_shadow_runtime_contract.cpp`
   - attachment snapshot 改为：
     - 如果 live attachment store 的 identity 完整度高于已发布快照，则优先返回 live 修补版；
   - 目标是排除“早期匿名 attachment snapshot 把后续热身份卡死”的假 blocker。

#### 28.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
   - 说明当前这轮新增 hook / summary / contract 选择逻辑均已编译落地
2. fresh runtime 验证（第一轮，新增 `attachedEffectInit* / attachedEffectDirect*` 计数）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_171401.json`
   - 关键结果：
     - `attachedEffectInitBindCount = 17`
     - `attachedEffectInitResolvedIdentityCount = 16`
     - `attachedEffectInitResolvedUnitCount = 16`
     - `attachedEffectInitResolvedHandleCount = 16`
     - `attachedEffectInitResolvedRawcodeCount = 16`
     - `attachedEffectDirectBindCount = 0`
     - `attachModelToPointBindCount = 17`
     - `attachModelToPointResolvedIdentityCount = 0`
     - `attachmentRigidCount = 2`
     - `contractAttachmentRigidCount = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
   - 同轮还确认：
     - `lastAttachedEffectInitChildSpritePtr == lastAttachModelToPointChildSpritePtr`
     - `lastAttachedEffectInitChildRuntimeModelPtr == lastAttachModelToPointChildRuntimeModelPtr`
3. fresh runtime 验证（第二轮，加入 live-attachment prefer 修补后）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_171928.json`
   - 关键结果：
     - `attachedEffectInitBindCount = 17`
     - `attachedEffectInitResolvedIdentityCount = 17`
     - `attachedEffectDirectBindCount = 0`
     - `attachModelToPointBindCount = 17`
     - `attachModelToPointResolvedIdentityCount = 0`
     - `attachmentRigidCount = 2`
     - `contractAttachmentRigidCount = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`

#### 28.3 这轮真正证明了什么

1. `0x6F6BB2C0` (`AttachedEffectInit`) 在当前 `model_runtime_probe` 里是热的
   - 而且它不是“空命中”：
     - 同一批 child sprite / child runtime 上，绝大多数样本已经能解析出 `unitPtr / jHandle / rawcode`
2. `0x6F6B9FF0` 不是当前这批 remaining anonymous attachment family 的热 caller
   - 因为 fresh 两轮里它都保持：
     - `attachedEffectDirectBindCount = 0`
3. 当前 blocker 已经不能再表述成“上游没有 identity”
   - 更准确的新结论是：
     - 至少 `AttachedEffectInit` 这条上游链已经拿到了 identity
     - 但 current remaining `attachmentRigidCount = 2` 这批匿名 attachment，仍然没有在 contract / semantic-core 末端体现出来
4. “published attachment snapshot 太早冻结成匿名版”不是唯一 primary blocker
   - 因为 live-attachment prefer 修补已经落地，但：
     - `contractAttachmentRigidCountWithAnyIdentity` 仍然是 `0`
     - `semanticCoreAttachmentRigidResolved` 仍然是 `0`
5. 当前最收敛的解释变成：
   - 要么这 2 条匿名 attachment rigid record 并不是 `AttachedEffectInit/AttachModelToPoint` 当前热到的那一批 child runtime
   - 要么 `NoteAttachmentRigidRecord -> AttachmentRigidRegistry -> RepairAttachmentRigidIdentity` 这条链对这 2 条 record 的 join key 仍然不对

#### 28.4 当前下一步

1. 下一轮不再继续深挖 `0x6F6B9FF0`
   - 当前 fresh 证据已经足够把它从 primary blocker 列表里降级
2. 下一轮应直接对齐这两类 runtime 指针：
   - `AttachedEffectInit / AttachModelToPoint` 当前热命中的 `childRuntimeModelPtr`
   - `AttachmentRigidRegistry / contract attachment` 当前匿名那 2 条 record 的 `root/owner/child runtime`
3. 推荐最小工程推进：
   - 在 `get_shadow_runtime_summary` 里直接补出“最新 attachment rigid raw/contract record”的 runtime pointer sample；
   - 或在 `NoteAttachmentRigidRecord(...)` 当场补一次 `ModelInstanceRegistry::findOwnerByRuntimeModel(...)` 的 source-side 观测/修补；
   - 目标不是再猜更高 caller，而是先确认 current anonymous attachment rigid 到底是不是这批已知有 identity 的 child runtime。

### 29. 2026-04-22 匿名 attachment rigid 已确认不是 `AttachedEffectInit/AttachModelToPoint` 那一族 runtime

#### 29.1 本轮新增 summary / probe 面

1. `war3_shadow_runtime_bridge.h/.cpp`
   - `get_shadow_runtime_summary` 新增 raw / contract attachment 的 runtime 观测：
     - `attachmentRigidChildRuntimeOwnerIdentityCount`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount`
     - `attachmentRigidRootRuntimeOwnerIdentityCount`
     - `contractAttachmentRigidChildRuntimeOwnerIdentityCount`
     - `contractAttachmentRigidOwnerRuntimeOwnerIdentityCount`
     - `contractAttachmentRigidRootRuntimeOwnerIdentityCount`
     - `attachmentRigidSample0/1{Root,Owner,Child}RuntimeModelPtr`
     - `contractAttachmentRigidSample0/1{Root,Owner,Child}RuntimeModelPtr`
   - 另外新增“是否命中当前 `AttachedEffectInit/AttachModelToPoint` child runtime”的硬计数：
     - `attachmentRigidChildRuntimeMatchesAttachedEffectInitCount`
     - `attachmentRigidChildRuntimeMatchesAttachModelToPointCount`
     - `contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount`
     - `contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount`
2. `war3_control_plane.cpp`
   - 将上述字段全部接进 pipe summary JSON

#### 29.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮都通过
   - 本机并行 `ninja` 仍容易触发 `cc1plus.exe` OOM，所以本轮继续固定串行编译
2. fresh runtime 验证（样本 runtime 暴露）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_223445.json`
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `attachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidSample0ChildRuntimeModelPtr = 437458652`
     - `attachmentRigidSample1ChildRuntimeModelPtr = 437456388`
     - `lastAttachedEffectInitChildRuntimeModelPtr = 759095252`
     - `lastAttachModelToPointChildRuntimeModelPtr = 759095252`
     - `semanticCoreAttachmentRigidResolved = 0`
3. fresh runtime 验证（把“是否同一批 child runtime”彻底钉死）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_223931.json`
   - 完成时间点：`2026-04-22 22:39:31.157 +08:00`
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `attachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidChildRuntimeMatchesAttachedEffectInitCount = 0`
     - `attachmentRigidChildRuntimeMatchesAttachModelToPointCount = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount = 0`
     - `contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount = 0`
     - `attachmentRigidSample0ChildRuntimeModelPtr = 424941276`
     - `attachmentRigidSample1ChildRuntimeModelPtr = 424939012`
     - `lastAttachedEffectInitChildRuntimeModelPtr = 781639636`
     - `lastAttachModelToPointChildRuntimeModelPtr = 781639636`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`
     - `semanticCoreFrameFresh = true`

#### 29.3 这轮新结论

1. 现在已经可以明确排除一个方向：
   - 当前 `attachmentRigidCount = 3` 这批匿名 attachment
   - **不是** `AttachedEffectInit / AttachModelToPoint` 当前热到的那批 child runtime
2. 结论之所以已经够硬，是因为：
   - raw / contract attachment 的 `child/root/owner runtime` 都没有 owner identity
   - 且它们和当前 `lastAttachedEffectInitChildRuntimeModelPtr / lastAttachModelToPointChildRuntimeModelPtr` 的硬匹配计数都是 `0`
3. 这意味着 blocker 进一步收敛为：
   - 当前匿名 attachment rigid 来自另一族 runtime tree
   - 而这族 runtime tree 在 `ModelInstanceRegistry::findOwnerByRuntimeModel(...)` 上完全匿名
4. 所以现在不能再把问题描述成：
   - “上游 identity 已经拿到，只是 contract 没接上”
   - 更准确的说法应变成：
   - “`AttachedEffectInit` 这条上游确实拿到了 identity，但 remaining anonymous attachment rigid 不是那条链产出的对象”

#### 29.4 当前 blocker 与下一步

1. 当前 primary blocker 已经改写为：
   - 要找到这 3 条匿名 attachment rigid 所属的 **另一条 runtime family** 的 owner/source 发布层
2. 下一轮不要继续做的事：
   - 不再继续围绕 `AttachedEffectInit`
   - 不再继续围绕 `AttachModelToPoint`
   - 不再把“live attachment snapshot 太早冻结”当 primary blocker
3. 下一轮最优先的推进方向：
   - 直接围绕 `NoteAttachmentRigidRecord(...)` 当前这条 local-point / controller 主路径
   - 继续补 source-side 观测，确认这 3 条匿名 attachment 的 `rootRuntime / ownerRuntime` 是否能在更上游对回：
     - controller owner
     - root sprite
     - widget / object
   - 简单说，下一轮要追的是“匿名 attachment 这族 runtime 自己的祖宗是谁”，而不是再验证 `AttachedEffectInit`。

### 30. 2026-04-22 匿名 attachment rigid 已确认既没有 arg-block identity，也没有 sprite binding

#### 30.1 本轮代码推进

1. `war3_model_hook.cpp/.h`
   - 先补了 `argBlock / arg4Block` identity-hint 观测与恢复：
     - `localPointArgBlockIdentityHintWriteCount`
     - `localPointArg4BlockIdentityHintWriteCount`
     - `lastArgBlockIdentityHintPtr/Offset`
     - `lastArg4BlockIdentityHintPtr/Offset`
   - 然后补了 `parentSprite` owner-hint 恢复：
     - `localPointParentSpriteIdentityHintWriteCount`
     - `lastParentSpriteIdentityHintSpritePtr`
     - `lastParentSpriteIdentityHintRuntimeModelPtr`
   - 最后又补了一层只读观测：
     - `localPointSpriteBoundCandidateWriteCount`
     - `lastSpriteBoundCandidateSpritePtr`
     - `lastSpriteBoundCandidateRuntimeModelPtr`
   - 这层只回答一件事：当前 anonymous attachment 的 `root/owner/child runtime` 自己到底有没有任何 `spritePtr` 绑定。
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 已把上述新字段全部接进 `get_shadow_runtime_summary` JSON。

#### 30.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮都通过。
   - 仍保持串行编译，避免本机并行 `ninja` 触发 `cc1plus.exe` OOM。
2. fresh runtime 验证（先试 `parentSprite` fallback）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_232247.json`
   - 完成时间点：`2026-04-22 23:22:47 +08:00`
   - 关键结果：
     - `attachmentRigidCount = 2`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCount = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `overrideLocalPointArgBlockIdentityHintWriteCount = 0`
     - `overrideLocalPointArg4BlockIdentityHintWriteCount = 0`
     - `overrideLocalPointParentSpriteIdentityHintWriteCount = 0`
     - `overrideLastParentSpriteIdentityHintSpritePtr = 0`
     - `overrideLastParentSpriteIdentityHintRuntimeModelPtr = 0`
3. fresh runtime 验证（再钉死“有没有 sprite binding”）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_232806.json`
   - 完成时间点：`2026-04-22 23:28:06 +08:00`
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `overrideLocalPointArgBlockIdentityHintWriteCount = 0`
     - `overrideLocalPointArg4BlockIdentityHintWriteCount = 0`
     - `overrideLocalPointSpriteBoundCandidateWriteCount = 0`
     - `overrideLocalPointParentSpriteIdentityHintWriteCount = 0`
     - `overrideLastSpriteBoundCandidateSpritePtr = 0`
     - `overrideLastSpriteBoundCandidateRuntimeModelPtr = 0`
     - `overrideLastParentSpriteIdentityHintSpritePtr = 0`
     - `overrideLastParentSpriteIdentityHintRuntimeModelPtr = 0`
     - `attachmentRigidSample0RootRuntimeModelPtr = 468955172`
     - `attachmentRigidSample0OwnerRuntimeModelPtr = 468954944`
     - `attachmentRigidSample0ChildRuntimeModelPtr = 437851868`
     - `attachmentRigidSample1RootRuntimeModelPtr = 468941096`
     - `attachmentRigidSample1OwnerRuntimeModelPtr = 468940868`
     - `attachmentRigidSample1ChildRuntimeModelPtr = 437849604`

#### 30.3 这轮新结论

1. 当前 anonymous attachment rigid 不只是“identity 还没补进 contract”。
2. 现在已经可以更硬地说：
   - `argBlock / arg4Block` 里没有直接可消费的 identity。
   - 当前 anonymous attachment 候选 runtime 也没有命中任何 `spritePtr` 绑定。
   - `parentSprite` owner-hint 这条修补链完全没有命中。
3. 所以 blocker 需要再次收敛：
   - 它已经不是 `AttachModelToPoint`
   - 也不是 `parentSprite`
   - 甚至不是“child runtime 已经绑到 sprite，只是还没顺着父链回 owner”
4. 更准确的当前说法应改成：
   - `local-point / controller` 这条主路径正在产出一族完全匿名、且当前不在 sprite registry 里的 runtime tree
   - 这族 runtime 的 owner/source 必须在 **sprite 之上的 object/source 发布层** 才能重新拿到

#### 30.4 当前 blocker 与下一步

1. 当前 primary blocker 已改写为：
   - 必须找到这族 anonymous runtime tree 在更上游第一次拿到 source/object identity 的位置
2. 下一轮不要再继续做的事：
   - 不再继续扩 `argBlock / arg4Block` 直接盲扫
   - 不再继续补 `parentSprite` repair
   - 不再继续围绕 `AttachModelToPoint / AttachedEffectInit` 打转
3. 下一轮最优先的推进方向：
   - 直接上抬到 `object/source -> runtime tree` 发布层
   - 优先围绕：
     - `0x6F12E900 / 0x6F12EB70` 的 caller 参数来源
     - 或能同时看到 `source object + root runtime` 的更高层入口
   - 简单说，下一轮要追的已经不是“这批匿名 attachment 属于哪个 sprite”，而是“这批匿名 attachment 在成为 runtime tree 之前到底属于哪个 source object”。

### 31. 2026-04-23 `CSpriteUber__PreRenderAndUpdatePosePalette` 的 `a3` 外部上下文已确认是真热，但它不是 logical owner

#### 31.1 本轮代码推进

1. `war3_model_hook.cpp`
   - 先补了一层缺失的 `sprite <-> runtimeModel` 回灌：
     - `RecordRuntimeModelBinding(...)` 现在会直接把 `spritePtr + runtimeModelPtr` 写回 `ModelInstanceRegistry::bindRuntimeModelToSprite(...)`
     - `RecordSpriteFramePoseFromSprite(...)` 也会在每帧 pose 路径上补写同一条绑定
   - 然后新增 `sprite-frame source probe`：
     - 在 `Hook_SpriteFrameUpdate / Hook_SpriteMiniFrameUpdate` 上直接观测 `0x6F182300 / 0x6F1820C0` 的 `a3`
     - 新摘要字段：
       - `spriteFrameSourceHintCount`
       - `spriteFrameSourceResolvedIdentityCount`
       - `lastSpriteFrameSourceObjectPtr`
       - `lastSpriteFrameSourceRuntimeModelPtr`
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 已把上述 `sprite-frame source` 观测字段接进 `get_shadow_runtime_summary`。

#### 31.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过。
2. fresh runtime 验证（补齐 sprite/runtime 绑定后）
   - 使用 `run_named_scenario(model_runtime_probe)` 做 pipe-first 验证
   - 当前 freshest 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260423_000432.json`
   - 完成时间点：`2026-04-23 00:04:32 +08:00`
3. 关键结果：
   - `attachmentRigidCount = 2`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `spriteFrameSourceHintCount = 6`
   - `spriteFrameSourceResolvedIdentityCount = 0`
   - `lastSpriteFrameSourceObjectPtr = 447086720`
   - `lastSpriteFrameSourceRuntimeModelPtr = 745430608`

#### 31.3 这轮新结论

1. 单纯补齐 `sprite <-> runtimeModel` 绑定回灌，仍然没有让 anonymous attachment rigid 认回 owner。
2. 现在已经可以更硬地说：
   - `0x6F182300` 的 `a3` 不是“完全不热”，它在当前场景下是真热的外部上下文。
   - 但把这块 `a3` 直接当 source object 去解，仍然完全拿不到 `unitPtr / jHandle / rawcode`。
3. 所以 blocker 需要再次收紧：
   - 现在不能再把 `CSpriteUber__PreRenderAndUpdatePosePalette` 的 `a3` 当成 logical owner 本体。
   - 更准确的当前说法应改成：
   - `0x6F182300` 已经拿到了某个“外部 transform / context provider”，但真正的 logical owner 还在构造并传入这块 context 的 caller 之上。

#### 31.4 当前 blocker 与下一步

1. 当前 primary blocker 已进一步改写为：
   - 必须找到 `0x6F182300` 的 caller 里“logical object / source object 与这块 a3 外部 context 同时在场”的位置
2. 下一轮不要再继续做的事：
   - 不再继续把 `0x6F182300` 的 `a3` 直接当 unit/widget 去试
   - 不再继续只堆 `sprite <-> runtimeModel` 被动绑定，希望 contract 自己长出 owner
3. 下一轮最优先的推进方向：
   - 继续上抬到 `CSpriteUber__PreRenderAndUpdatePosePalette` caller
   - 目标固定为拿到：
     - logical/source object
     - 当前 `CSpriteUber` / root runtime
     - 以及 caller 传下来的外部 context
   - 简单说，下一轮要追的已经不是“`a3` 是不是 owner”，而是“谁在更上面把 `a3` 这块外部 context 塞给了 sprite prerender，同时手里还拿着真正的 owner”。

### 32. 2026-04-23 已确认 render-layer 的 identity recover 不是写在 `WorldObjectEntry_Render`，而是更早就已经写好

#### 32.1 本轮代码推进

1. `war3_model_registry.cpp`
   - 在 `ModelInstanceRegistry::noteInstanceIdentityLocked(...)` 里补上了：
     - 当 render-side 已经通过 `unitPtr -> sprite -> runtimeModel` 推出 root runtime 后，
     - 也像 `bindSpriteToInstance / bindRuntimeModelToSprite` 一样继续调用
       `propagateRuntimeOwnerIdentityLocked(...)`
   - 这一步的目的很单纯：
     - 验证 render-side 已知 owner 是否只是“没正式发布进 runtime-owner map”。
2. `war3_hook_render_identity.cpp/.h`
   - 新增 `RenderIdentityLifecycleProbeSummary`
   - 在 `Hook_WorldObjectEntry_Render` 里直接记录：
     - `worldObjectEntryRenderCallCount`
     - `worldObjectEntryRenderSceneNodeReadyBeforeCount`
     - `worldObjectEntryRenderSceneNodeReadyAfterCount`
     - `worldObjectEntryRenderSceneNodeFilledByCallCount`
     - `worldObjectEntryRenderSceneNodeChangedCount`
     - 以及 latest `entry / sceneNode before / sceneNode after` sample
3. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 已把上述 lifecycle probe 接进 `get_shadow_runtime_summary`，让 control-plane 能直接回答：
     - `entry + 0x20(sceneNode)` 是进函数前就有，还是函数内才写进去。

#### 32.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过。
2. render-side owner 传播补丁验证
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260423_render_owner_bridge.json`
   - 完成时间点：`2026-04-23 00:12:28 +08:00`
   - 关键结果：
     - `runtimeOwnerIdentityCount: 19 -> 22`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
3. `WorldObjectEntry_Render` lifecycle probe 验证
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260423_render_identity_lifecycle.json`
   - 完成时间点：`2026-04-23 00:17:02 +08:00`
   - 关键结果：
     - `worldObjectEntryRenderCallCount = 59`
     - `worldObjectEntryRenderSceneNodeReadyBeforeCount = 59`
     - `worldObjectEntryRenderSceneNodeReadyAfterCount = 59`
     - `worldObjectEntryRenderSceneNodeFilledByCallCount = 0`
     - `worldObjectEntryRenderSceneNodeChangedCount = 0`
     - `lastWorldObjectEntryRenderSceneNodeBeforePtr == lastWorldObjectEntryRenderSceneNodeAfterPtr`

#### 32.3 这轮新结论

1. render-side 这条链不是完全无效：
   - 补齐 `noteInstanceIdentityLocked -> propagateRuntimeOwnerIdentityLocked` 后，
   - `runtimeOwnerIdentityCount` 确实从 `19` 抬到了 `22`。
2. 但 current anonymous attachment rigid 仍然完全吃不到这批 owner：
   - `attachmentRigid*OwnerIdentityCount` 仍为 `0`
   - `contractAttachmentRigidCountWithAnyIdentity` 仍为 `0`
   - `semanticCoreAttachmentRigidResolved` 仍为 `0`
3. 更关键的 hard conclusion 已经拿到：
   - render-layer 用来追回逻辑对象的关键键之一 `worldObjectEntry + 0x20 = sceneNode`
   - 在 `WorldObjectEntry_Render` 进来之前就已经全部写好
   - 而且这层函数内不会把它从空写成非空，也不会改写成别的 sceneNode
4. 这意味着：
   - 当前 owner/source 的真正写入点不在 `WorldObjectEntry_Render`
   - 也不是这层 pre-render virtual call 在临时补 sceneNode
   - 必须继续上抬到：
     - `WorldObjectListEntry` 构建层
     - 或更上游的 `source object -> sprite/runtime` 绑定层

#### 32.4 当前 blocker 与下一步

1. 当前 primary blocker 已改写为：
   - 不是“render-layer 不知道对象是谁”
   - 而是“anonymous attachment 这族 runtime tree 根本还没 join 到 render-layer 那套 owner key 上”
2. 当前最优先下一步固定为：
   - 直接查 `WorldObjectListEntry + 0x14` 和 `WorldObjectEntry + 0x20` 更早的写入点
   - 重点不再放在 `WorldObjectEntry_Render`
   - 而是放在：
     - world-group/list build 之前后
     - `0x6F6BD110 / 0x6F185250` 这类 `source object -> sprite/runtime` 绑定层
3. 一句话总结这轮结论：
   - “渲染层能追回逻辑对象”这件事是真的；
   - 但它依赖的 key 在 `WorldObjectEntry_Render` 前就已存在，
   - 所以 anonymous attachment 的 owner 缺口一定还在更早的写入层。

### 33. 2026-04-23 已验证“render 真消费的 entry 全是 zero-owner”，且 `0x6F182300` 的 `a3/sourceObject` 也不是 render-layer object

#### 33.1 本轮代码推进

1. `war3_hook_render_identity.cpp/.h`
   - 在 `Hook_WorldObjectListEntry_Write(0x6F0CB110)` 侧新增 recent-write 记账；
   - 在 `Hook_WorldObjectEntry_Render` 侧直接对回“当前 entry 最近一次 list write 的 ownerHint”；
   - 新增 control-plane 字段：
     - `worldObjectEntryRenderKnownListOwnerHintZeroCount`
     - `worldObjectEntryRenderKnownListOwnerHintNonzeroCount`
     - `worldObjectEntryRenderUnknownListOwnerHintCount`
     - `lastWorldObjectEntryRenderResolvedListOwnerHintValue`
2. `war3_model_hook.cpp/.h`
   - 在 `TryResolveSourceObjectIdentity(...)` 里新增 render-layer fallback：
     - 先尝试把 `sourceObjectPtr` 当 `worldObjectEntry`
     - 再尝试读 `sourceObjectPtr + 0x20(sceneNode)` 回查 `ShadowObjectRegistry`
   - 新增 control-plane 字段：
     - `sourceObjectRenderBridgeResolvedByEntryCount`
     - `sourceObjectRenderBridgeResolvedBySceneNodeCount`
     - latest `sourceObjectPtr / sceneNode` sample
3. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 已把上述两组新计数全部接入 `get_shadow_runtime_summary`
   - 现在 control-plane 能直接回答：
     - “真正进入 render 的 entry 最近一次 list write 是 0 还是非 0”
     - “`0x6F182300` 的 `a3/sourceObject` 能不能直接按 render-layer object 认出来”

#### 33.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮均通过
2. refined list-writer 对账工件
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_list_write_refine_20260423_023030.json`
   - 完成时间点：`2026-04-23 02:30:30.770 +08:00`
   - 关键结果：
     - `worldObjectListEntryWriteCallCount = 59`
     - `worldObjectListEntryWriteOwnerHintZeroCount = 59`
     - `worldObjectListEntryWriteOwnerHintNonzeroCount = 0`
     - `worldObjectEntryRenderCallCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintZeroCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintNonzeroCount = 0`
     - `worldObjectEntryRenderUnknownListOwnerHintCount = 0`
3. source-object render-bridge fallback 工件
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_source_render_bridge_20260423_023835.json`
   - 完成时间点：`2026-04-23 02:38:35.842 +08:00`
   - 关键结果：
     - `worldObjectListEntryWriteCallCount = 76`
     - `worldObjectListEntryWriteOwnerHintZeroCount = 59`
     - `worldObjectListEntryWriteOwnerHintNonzeroCount = 17`
     - `worldObjectEntryRenderCallCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintZeroCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintNonzeroCount = 0`
     - `worldObjectEntryRenderUnknownListOwnerHintCount = 0`
     - `sourceObjectRenderBridgeResolvedByEntryCount = 0`
     - `sourceObjectRenderBridgeResolvedBySceneNodeCount = 0`
     - `spriteFrameSourceHintCount = 29`
     - `spriteFrameSourceResolvedIdentityCount = 0`
     - `attachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`

#### 33.3 这轮新结论

1. 现在已经不是“估计 render 消费到的大多是 zero-owner entry”，而是 control-plane 直接坐实了：
   - 当前 `model_runtime_probe` 下真正进入 `WorldObjectEntry_Render` 的 `59` 条 entry
   - 最近一次 list write 全都能对回
   - 而且这 `59` 条全部都是 `ownerHint = 0`
2. 同时也坐实了另一件关键事：
   - list writer 整体并不是“永远没有非零 owner”
   - 第二轮 fresh 里仍然有 `17` 条 nonzero ownerHint write
   - 但这些 nonzero owner entry 没有进入当前这批真正被 render 消费的 `59` 条 entry
3. 用户提出的“直接把渲染层认对象方法搬到上游 sourceObject 上试一次”这条路，已经被本轮实证排掉：
   - `sourceObjectRenderBridgeResolvedByEntryCount = 0`
   - `sourceObjectRenderBridgeResolvedBySceneNodeCount = 0`
   - `spriteFrameSourceHintCount = 29` 但 `spriteFrameSourceResolvedIdentityCount = 0`
4. 所以当前更准确的 hard conclusion 是：
   - `0x6F182300` 的 `a3/sourceObject` 不是当前 render-layer 那种 `worldObjectEntry`
   - 它也不是一个“自身带着可直接回查 sceneNode”的 render object
   - 它更像是一块外部 context / provider，而不是 logical owner 本体

#### 33.4 当前 blocker 与下一步

1. 当前 primary blocker 再次收敛：
   - 不是 `WorldObjectListEntry` writer 不清楚
   - 不是 render-layer recover 方法没试过
   - 而是 `CSpriteUber__PreRenderAndUpdatePosePalette(0x6F182300)` 这一层拿到的 `a3` 仍然只是外部 context，真正 owner 还在 caller 之上
2. 下一轮固定主线：
   - 不再继续把 `a3/sourceObject` 当 render object / widget / unit 本体去试
   - 直接继续追 `0x6F182300 / 0x6F1820C0` 的 caller
   - 目标是找到“谁在 caller 层同时持有：
     - logical/source object
     - sprite/root runtime
     - 以及传入 pre-render 的这块 `a3` context”
3. 一句话总结这轮结论：
   - render-layer 那张“对象胸牌”确实存在，
   - 但当前 anonymous attachment 所经过的 `0x6F182300` 这层拿到的不是那张胸牌，
   - 所以真正 owner 仍然藏在更上面的 context producer。

### 9.10.46 Render-side TLS owner backfill round: current render/world-object TLS and dispatch semantic TLS are both absent at the model-hook timing, so anonymous attachment owner still must come from an upstream producer (2026-04-23 03:15:16.465 +08:00)

#### 9.10.46.1 本轮实现

1. `src/d3d9/war3/model/war3_model_hook.cpp`
   - 新增统一 helper：
     - `TryResolveCurrentRenderOwnerHint(...)`
     - `MergeAttachmentIdentityFromRender(...)`
   - 先尝试从以下现有 render-side TLS/context 反灌 owner：
     - `War3Renderer::GetCurrentWorldObjectEntry/GetCurrentSceneNode`
     - `War3RenderState::GetTlsShadowSemanticState()`
     - `render::GetCurrentBatchObject()`
   - 已把这条 fallback 接进：
     - `RecordSpriteHostOwnerBinding(...)`
     - `RecordAttachedEffectInitOwnerBinding(...)`
     - `RecordAttachedEffectDirectOwnerBinding(...)`
     - `TryResolveAttachModelToPointOwner(...)`
     - `MaybeRecordSpriteFrameSourceIdentity(...)`
     - `NoteAttachmentRigidRecord(...)`
   - 若 render-side context 命中，会直接把：
     - `worldObjectEntry`
     - `sceneNode`
     - `unitPtr`
     - `jHandle`
     - `rawcode`
     回灌到 `ModelInstanceRegistry / AttachmentRigidRegistry / ShadowRuntimeBridge`
2. `src/d3d9/war3/model/war3_model_hook.h`
   - 新增观测字段：
     - `currentRenderIdentityHintCount`
     - `currentRenderIdentityResolvedCount`
     - `lastCurrentRenderIdentityWorldObjectEntryPtr`
     - `lastCurrentRenderIdentitySceneNodePtr`
     - `lastCurrentRenderIdentityUnitPtr`
3. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`
   - 已把上述新字段接入 bridge summary
4. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 已输出上述新字段
5. IDA 注释/命名补齐
   - 已补：
     - `CSpriteUber_PreRenderAndUpdatePosePalette(0x6F182300)`
     - `CSpriteMini_PreRenderAndUpdatePosePalette(0x6F1820C0)`
     - `CreateSpriteAndBindSourceObject(0x6F6BD110)`
     - `CreateSpriteRuntimeFromSourceObject(0x6F185250)`
     - `AttachModelToPoint_CreateChildSpriteRuntimeIfNeeded(0x6F182C60)`
     - `WorldObjectList_AppendEntryWithOwnerHint(0x6F0CB110)`
   - 并对 `0x6F0CB000 / 0x6F0CB4A0 / 0x6F0CAAE0 / 0x6F0CAA90` 补了说明注释

#### 9.10.46.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮均通过
2. render world-object TLS backfill 工件
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_tls_owner_bridge_20260423_0310.json`
   - 完成时间点：`2026-04-23 03:10:00 +08:00`（artifact 时间名）
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `runtimeOwnerIdentityCount = 21`
     - `completeIdentityCount = 21`
     - `currentRenderIdentityHintCount = 68`
     - `currentRenderIdentityResolvedCount = 0`
3. dispatch semantic/current-batch TLS backfill 工件
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_tls_semantic_owner_bridge_20260423_0318.json`
   - 完成时间点：`2026-04-23 03:18:00 +08:00`（artifact 时间名）
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `runtimeOwnerIdentityCount = 21`
     - `completeIdentityCount = 21`
     - `currentRenderIdentityHintCount = 69`
     - `currentRenderIdentityResolvedCount = 0`
     - `lastCurrentRenderIdentityWorldObjectEntryPtr = 0`
     - `lastCurrentRenderIdentitySceneNodePtr = 0`
     - `lastCurrentRenderIdentityUnitPtr = 0`

#### 9.10.46.3 这轮新结论

1. 这轮不是“render fallback 没接上”，而是 fresh probe 已直接证明：
   - model/attachment hook 发生时，
   - `War3Renderer` 的当前 `worldObjectEntry/sceneNode` TLS 不在场
   - `War3RenderState` 的 `TlsShadowSemanticState` / `CurrentBatchObject` 也同样不在场
2. 因为 `currentRenderIdentityHintCount > 0` 但 `currentRenderIdentityResolvedCount = 0`，所以现在可以把下面这条线正式排除：
   - “anonymous attachment runtime 其实已经在当前 render hot path 里，只是我们之前没把 TLS context 接回来”
3. 当前更准确的 hard conclusion 已收敛为：
   - anonymous attachment 的 owner 身份不在当前 render hot path 的现成 TLS/context 里
   - 它必须继续从更上游的 producer / caller 链拿
4. 同时也补强了另一件事：
   - `CreateSpriteAndBindSourceObject(0x6F6BD110)` / `CreateSpriteRuntimeFromSourceObject(0x6F185250)` 这一组仍比 render-layer fallback 更像真正的 owner producer 候选
   - `0x6F0CB000 / 0x6F0CB110` 当前更像 world-object list 的遍历/写入消费层，不是缺失 owner 的最终来源点

#### 9.10.46.4 当前 blocker 与下一步

1. 当前 blocker 继续收紧：
   - 不是 render-layer recover
   - 不是 current world-object TLS 生命周期太短
   - 也不是 ExecBatch semantic/current-batch TLS 没利用上
   - 而是 owner 身份确实还在 `0x6F182300 / 0x6F1820C0` 之上的 producer/caller
2. 下一轮固定主线：
   - 不再继续做任何 render-side TLS backfill 实验
   - 直接沿 `CreateSpriteAndBindSourceObject(0x6F6BD110)`、`CreateSpriteRuntimeFromSourceObject(0x6F185250)`、`CSpriteUber_PreRenderAndUpdatePosePalette(0x6F182300)` 的 caller / vtable / producer 链继续上抬
   - 目标是找到“第一次同时持有：
     - logical/source object
     - sprite/root runtime
     - prerender/update context”
     的 authoritative producer

### 9.10.47 Source-object runtime publication round: the producer-side sourceObject contract is now live, but the 3 anonymous attachment records still do not join that producer runtime family (2026-04-23 03:36:43.793 +08:00)

#### 9.10.47.1 本轮实现

1. `src/d3d9/war3/model/war3_model_registry.h/.cpp`
   - `ModelInstanceRecord` 新增：
     - `sourceObjectPtr`
     - `sourceSpriteObjectPtr`
   - `AttachmentRigidRecord` 新增：
     - `sourceObjectPtr`
     - `sourceSpriteObjectPtr`
   - `ModelInstanceRegistry` 新增：
     - `noteRuntimeSourceObject(...)`
     - `runtimeSourceObjectCount()`
   - `AttachmentRigidRegistry::noteAttachmentRigid(...)` 现在会把 source-object bloodline 一起存档
2. `src/d3d9/war3/model/war3_model_hook.cpp/.h`
   - 新增统一 helper：
     - `TryReadSourceSpriteObjectPtr(...)`
     - `PublishRuntimeSourceObject(...)`
   - 已把 source-object runtime publication 接进：
     - `RecordSpriteHostOwnerBinding(...)`
     - `RecordAttachedEffectInitOwnerBinding(...)`
     - `RecordAttachedEffectDirectOwnerBinding(...)`
     - `RecordAttachModelToPointOwnerBinding(...)`
     - `MaybeRecordSpriteFrameSourceIdentity(...)`
   - `NoteAttachmentRigidRecord(...)` 现在会：
     - 先尝试从 `child/owner/root runtime` 回收 `sourceObjectPtr/sourceSpriteObjectPtr`
     - 若回收到 source-object，会再补做一次 `TryResolveSourceObjectIdentity(...)`
     - 并把 source-object 一路写进 `AttachmentRigidRegistry`
   - 新增 control-plane counters：
     - `runtimeSourceObjectPublishCount`
     - `attachmentRigidPublishedWithSourceObjectCount`
     - `attachmentRigidSourceObjectFromChildRuntimeCount`
     - `attachmentRigidSourceObjectFromOwnerRuntimeCount`
     - `attachmentRigidSourceObjectFromRootRuntimeCount`
     - 以及 `lastRuntimeSourceObjectPtr / lastRuntimeSourceRuntimeModelPtr / lastAttachmentRigidSourceObjectPtr`
3. `src/d3d9/war3/shadow/war3_shadow_runtime_contract.h/.cpp`
   - `ShadowAttachmentRigidRecord` 新增：
     - `sourceObjectPtr`
     - `sourceSpriteObjectPtr`
   - `ConvertAttachmentRigid(...)` 已将 raw attachment 的 source-object bloodline 穿入 contract
4. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`
   - summary 新增：
     - `runtimeSourceObjectCount`
     - `attachmentRigidCountWithSourceObject`
     - `contractAttachmentRigidCountWithSourceObject`
     - attachment sample 的 `SourceObjectPtr`
5. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 已暴露上述 source-object 相关字段
6. IDA 命名与注释
   - 已新增：
     - `SourceObject_GetSpriteSourceObject(0x6F6A0AD0)`
   - 结论：
     - 该函数不是复杂 builder，只是返回 `sourceObject + 0x28` 的 inner sprite-source object
     - `CreateSpriteAndBindSourceObject(0x6F6BD110)` 当前手里其实同时有：
       - outer `sourceObjectPtr`
       - inner sprite-source object
       - 新建 runtime sprite / runtimeModel

#### 9.10.47.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_source_object_producer_20260423_033623.json`
   - 完成时间点：`2026-04-23 03:36:23 +08:00`（artifact 时间名）
3. 关键输出
   - producer/source-object side：
     - `runtimeSourceObjectCount = 43`
     - `runtimeSourceObjectPublishCount = 49`
     - `lastRuntimeSourceObjectPtr = 455868544`
     - `lastRuntimeSourceRuntimeModelPtr = 787416552`
     - `lastRuntimeSourceSpriteObjectPtr = 3839098727`
   - anonymous attachment side：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithSourceObject = 0`
     - `attachmentRigidPublishedWithSourceObjectCount = 0`
     - `attachmentRigidSourceObjectFromChildRuntimeCount = 0`
     - `attachmentRigidSourceObjectFromOwnerRuntimeCount = 0`
     - `attachmentRigidSourceObjectFromRootRuntimeCount = 0`
     - `attachmentRigidSample0SourceObjectPtr = 0`
     - `attachmentRigidSample1SourceObjectPtr = 0`
   - contract side：
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithSourceObject = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidSample0SourceObjectPtr = 0`
     - `contractAttachmentRigidSample1SourceObjectPtr = 0`
   - runtime correctness：
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`

#### 9.10.47.3 这轮新结论

1. 现在可以正式确认：
   - `CreateSpriteAndBindSourceObject / CSpriteUber sprite-frame source` 这一族 producer 的 source-object publication 是真的活着的
   - 这不是“当前 producer 根本没跑”或者“sourceObject contract 没接到 registry”
2. 但同一轮 fresh probe 也更明确地排掉了一条假设：
   - 当前那 `3` 条 anonymous attachment rigid 并不是这条 producer runtime family 的后代
   - 证据是：
     - `runtimeSourceObjectCount = 43`
     - 但 `attachmentRigidCountWithSourceObject = 0`
     - `contractAttachmentRigidCountWithSourceObject = 0`
     - `attachmentRigidSourceObjectFromChild/Owner/RootRuntimeCount` 全部为 `0`
3. 所以 blocker 又收紧了一层：
   - 不是“当前 repo 没有一条 live owner/source producer”
   - 而是“anonymous attachment runtime tree 属于另一族 producer/caller”
4. 这也解释了为什么：
   - `attachedEffectInitResolvedIdentityCount = 18`
   - `runtimeSourceObjectPublishCount = 49`
   - 但 attachment rigid 仍完全匿名
   - 因为它们根本不是同一批 runtime family

#### 9.10.47.4 当前 blocker 与下一步

1. 当前 blocker 已更新为：
   - `CreateSpriteAndBindSourceObject / CreateSpriteRuntimeFromSourceObject / CSpriteUber_PreRenderAndUpdatePosePalette` 这一族 producer 已被证明是 live，但不覆盖当前 3 条 anonymous attachment
   - 真正缺失 owner 的，是另一族生成 `attachmentRigid sample0/1 rootRuntime + ownerRuntime + childRuntime` 的 producer/caller
2. 下一轮固定主线：
   - 不再继续在当前 source-object producer family 里猜 owner
   - 直接围绕这 `3` 条 anonymous attachment 的 `rootRuntimeModelPtr / ownerRuntimeModelPtr / childRuntimeModelPtr` 去追“谁生成了这组 runtime tree”
   - 优先查：
     - local-point/controller 所在 runtime family 的上游 producer
     - 以及不经过 `CreateSpriteAndBindSourceObject` 的 child runtime 创建路径

### 9.10.48 Source-object propagation + direct `CreateSpriteRuntime` publication round: both hypotheses have now been live-tested and both fail to light up the 3 anonymous attachment records (2026-04-23 03:49:25.203 +08:00)

#### 9.10.48.1 本轮改动

1. `src/d3d9/war3/model/war3_model_registry.h/.cpp`
   - 新增 `ModelInstanceRegistry::propagateRuntimeSourceObjectLocked(...)`
   - `noteRuntimeSourceObject(...)` 在写入 root runtime 的 `sourceObjectPtr/sourceSpriteObjectPtr` 后，会沿 runtime tree 把这条 bloodline 传播到 descendant runtime
   - 本轮传播故意只写：
     - `sourceObjectPtr`
     - `sourceSpriteObjectPtr`
   - **不**覆盖 child runtime 自己已有的：
     - `spritePtr`
     - `worldObjectEntry/sceneNode/unitPtr`
     - 其他 identity/model 字段
   - 目的只是证伪/证实“anonymous attachment 会不会只是没继承到 source 血缘”
2. `src/d3d9/war3/model/war3_model_hook.cpp`
   - 在 `RecordRuntimeModelBinding(...)` / `Hook_CreateSpriteRuntime(0x6F185250)` 正式补上：
     - `PublishRuntimeSourceObject(runtimeModelPtr, spritePtr, sourcePtr, TryReadSourceSpriteObjectPtr(sourcePtr))`
   - 这样即使 child runtime 直接走 `CreateSpriteRuntimeFromSourceObject`，不经过 `CreateSpriteAndBindSourceObject(0x6F6BD110)`，也会把 `sourcePtr` 发布进当前 source-object contract
3. IDA 注释补齐（避免下一轮重复拆 `0x6F6BD110` 身边 helper）
   - `0x6F185A10`
   - `0x6F185A40`
   - `0x6F1859D0`
   - `0x6F185A60`
   - 当前都已补成“thin wrapper over source-object getter”的注释，说明 `SpriteHost_CreateSpriteAndBindSourceObject` 在创建 sprite 时会从 source object 拉取 vec3/float/36-byte transform/flag-word 这类初始化量

#### 9.10.48.2 本轮验证

1. `ninja -C build32 -j1`
   - propagation round：通过
   - direct-`CreateSpriteRuntime` publication round：通过
2. fresh 工件
   - 中间证伪工件：
     - `AutoTest/artifacts/codex_model_runtime_probe_source_object_propagation_20260423_034542.json`
   - 最终 fresh 工件：
     - `AutoTest/artifacts/codex_model_runtime_probe_create_runtime_source_20260423_034828.json`
3. 最终 fresh 关键输出
   - source-object side：
     - `runtimeSourceObjectCount = 42`
     - `runtimeSourceObjectPublishCount = 48`
   - anonymous attachment side：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithSourceObject = 0`
     - `attachmentRigidPublishedWithSourceObjectCount = 0`
     - `attachmentRigidSourceObjectFromChildRuntimeCount = 0`
     - `attachmentRigidSourceObjectFromOwnerRuntimeCount = 0`
     - `attachmentRigidSourceObjectFromRootRuntimeCount = 0`
     - `attachmentRigidSample0SourceObjectPtr = 0`
     - `attachmentRigidSample1SourceObjectPtr = 0`
   - contract side：
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithSourceObject = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidSample0SourceObjectPtr = 0`
     - `contractAttachmentRigidSample1SourceObjectPtr = 0`
   - runtime correctness：
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`
   - sample runtime trio（fresh sample0）：
     - `attachmentRigidSample0RootRuntimeModelPtr = 457945124`
     - `attachmentRigidSample0OwnerRuntimeModelPtr = 457944896`
     - `attachmentRigidSample0ChildRuntimeModelPtr = 427038428`

#### 9.10.48.3 这轮新结论

1. “source-object 只写到 root runtime、没沿 runtime tree 传播下去”这条假设，已经被 live 证伪。
   - 因为传播补完后：
     - `attachmentRigidSourceObjectFromChild/Owner/RootRuntimeCount` 仍全是 `0`
     - attachment raw/contract sample 仍全是 `0 sourceObjectPtr`
2. “anonymous attachment 只是走了 `0x6F185250` direct path，但我们没在那里发布 sourceObject”这条假设，也已经被 live 证伪。
   - 因为在 `Hook_CreateSpriteRuntime(0x6F185250)` 补完 source publication 后：
     - `attachmentRigidCountWithSourceObject` 仍是 `0`
     - `contractAttachmentRigidCountWithSourceObject` 仍是 `0`
3. 因而本轮 hard conclusion 比上一轮更强：
   - 当前那 `3` 条 anonymous attachment rigid **不是**
     - “当前 source-object producer family 的 child runtime，只是没继承到 source”
   - 也 **不是**
     - “漏了 direct `CreateSpriteRuntime` source publication”
4. 也就是说，当前 blocker 已经可以更硬地表述为：
   - 这 `3` 条 anonymous attachment runtime trio 来自 **另一族 runtime producer/caller**
   - 而且这族 producer 当前既不走：
     - `CreateSpriteAndBindSourceObject(0x6F6BD110)`
     - 也不走我们已接入 source publication 的 `CreateSpriteRuntimeFromSourceObject(0x6F185250)` 语义面

#### 9.10.48.4 当前 blocker 与下一步

1. 当前 blocker 进一步收敛为：
   - 必须直接围绕 fresh sample runtime trio
     - `rootRuntimeModelPtr = 457945124`
     - `ownerRuntimeModelPtr = 457944896`
     - `childRuntimeModelPtr = 427038428`
     去找“是谁生成了这棵 anonymous runtime tree”
2. 下一轮固定主线：
   - 不再继续补 source propagation / source publication 同类实验
   - 不再继续在 `0x6F6BD110 / 0x6F185250` 这一族 producer 上兜圈
   - 直接上抬去查：
     - local-point/controller 所在 runtime family 的上游 producer
     - 以及不经过当前 source-object contract 的 child runtime 创建路径

### 9.10.49 Child-link `+0x0C source meta` round: live probe still resolves 0, so anonymous attachment owner is not directly recoverable from the chosen child-link slot itself (2026-04-23 03:58:58.069 +08:00)

#### 9.10.49.1 本轮改动

1. `src/d3d9/war3/model/war3_model_hook.cpp/.h`
   - `RuntimeChildLinkProbeRecord` 新增：
     - `sourceMeta`
   - `CollectDirectChildRuntimeLinks(...)` 现在会把 `linkNode + 0x0C` 同时记成：
     - `sourceMeta`
     - 现有兼容字段 `tag`
   - 在 local-point chosen child-link 上新增一个**只在高置信成功时才算命中**的 probe：
     - 仅当 `linkNode + 0x0C` 可读、且 `TryResolveSourceObjectIdentity(...)` 成功时
     - 才会记：
       - `localPointChildSourceMetaIdentityHintWriteCount`
       - `lastChildSourceMetaPtr`
       - `lastChildSourceMetaRuntimeModelPtr`
   - 这轮故意没有把 `linkNode + 0x0C` 生吞成主 contract，只把它当作 live validation probe，避免把低置信度 raw 值污染 attachment identity
2. `src/d3d9/war3/render/war3_shadow_runtime_bridge.*` / `src/d3d9/war3/tools/war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 新增暴露：
     - `overrideLocalPointChildSourceMetaIdentityHintWriteCount`
     - `overrideLastChildSourceMetaPtr`
     - `overrideLastChildSourceMetaRuntimeModelPtr`

#### 9.10.49.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_source_meta_20260423_035849.json`
3. 关键输出
   - child-link `+0x0C source meta` probe：
     - `overrideLocalPointChildSourceMetaIdentityHintWriteCount = 0`
     - `overrideLastChildSourceMetaPtr = 0`
     - `overrideLastChildSourceMetaRuntimeModelPtr = 0`
   - attachment side：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithSourceObject = 0`
     - `attachmentRigidSample0SourceObjectPtr = 0`
   - contract side：
     - `contractAttachmentRigidCountWithSourceObject = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidSample0SourceObjectPtr = 0`
   - runtime correctness：
     - `semanticCoreAttachmentRigidResolved = 0`
   - fresh sample runtime trio：
     - `attachmentRigidSample0RootRuntimeModelPtr = 463319076`
     - `attachmentRigidSample0OwnerRuntimeModelPtr = 463318848`
     - `attachmentRigidSample0ChildRuntimeModelPtr = 432412380`

#### 9.10.49.3 这轮新结论

1. `linkNode + 0x0C` 至少在当前 `model_runtime_probe` / chosen child-link 这条 live 路径上，并没有直接暴露一个可被 `TryResolveSourceObjectIdentity(...)` 消费的 owner/source object。
2. 这意味着当前 anonymous attachment 的 owner 缺口又可以再排掉一层：
   - 不是“owner/source 已经直接躺在 chosen child-link 的 `+0x0C source meta` 槽里，只是我们没读出来”
3. 因而当前 hard conclusion 再次收紧为：
   - anonymous attachment 的 owner/source 既不在当前 source-object producer family
   - 也不在 chosen child-link 自身这个可直接消费的 `+0x0C` 槽里
4. 当前更合理的解释是：
   - 真正缺失的 logical owner 仍然在 local-point/controller 所在 runtime family **更上游的 producer/caller 链**
   - 而不是在当前 child-link data slot 里直接可拿

#### 9.10.49.4 当前 blocker 与下一步

1. 当前 blocker 已更新为：
   - 不能再继续靠
     - source propagation
     - direct `0x6F185250` publication
     - child-link `+0x0C source meta`
     这类“就地补链”方法猜 owner
2. 下一轮固定主线：
   - 直接围绕 fresh sample runtime trio
     - `rootRuntimeModelPtr = 463319076`
     - `ownerRuntimeModelPtr = 463318848`
     - `childRuntimeModelPtr = 432412380`
   - 上抬到 local-point/controller 所在 runtime family 的 producer/caller，找“第一次同时持有 anonymous runtime tree 与 logical owner/source”的位置

### 9.10.50 Runtime create provenance + direct init fallback round: hot `0x6F12A5C0` creates are now proven live and IDA-confirmed to come through `0x6F12A3C0`, but the current anonymous attachment trio still has zero create provenance even after adding `0x6F130D90` fallback (2026-04-23 04:48:09.808 +08:00)

#### 9.10.50.1 本轮改动

1. `src/d3d9/war3/model/war3_model_registry.h/.cpp`
   - `ModelInstanceRecord` 新增 runtime create provenance：
     - `runtimeCreatorModelDataPtr`
     - `runtimeCreatorCallerRva`
   - `ModelInstanceRegistry` 新增：
     - `noteRuntimeCreationProvenance(...)`
     - `runtimeCreationProvenanceCount()`
2. `src/d3d9/war3/model/war3_model_hook.cpp/.h`
   - 新增 `Hook_PromoteRuntimeModel(0x6F12A5C0)`：
     - 记录 `runtimeModelPtr`
     - 记录 `modelDataPtr`
     - 记录 caller return RVA
   - 新增 `Hook_RuntimeInitFromModelData(0x6F130D90)` fallback：
     - 只在该 runtime 还没有 create provenance 时补发
     - 用来验证是否存在“绕开 `0x6F12A5C0`、直接走 runtime copy/init”的创建路径
   - `get_shadow_runtime_summary` / bridge / control-plane 新增：
     - `runtimeModelCreate*`
     - `runtimeModelInitCopy*`
     - `attachmentRigid*CreateCallerKnownCount`
     - `attachmentRigidSample{0,1}*CreateCallerRva`
     - `lastRuntimeModelCreate*`
     - `lastRuntimeModelInit*`
3. IDA 命名/注释回写
   - `0x6F12A3C0 -> ModelHandle_TryResolveRuntimeModel`
   - `0x6F12A5C0 -> CModelData_PromoteToRuntimeModel`
   - `0x6F130D90 -> CModelComplex_CopyFromModelData`
   - `0x6F131F60 -> CModelComplex_BuildChildRuntimeModelLinks`

#### 9.10.50.2 本轮验证

1. `ninja -C build32 -j1`
   - `0x6F12A5C0 provenance` round：通过
   - `0x6F130D90 fallback init` round：通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_create_provenance_20260423_044138.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_init_fallback_20260423_044711.json`
3. 关键输出
   - `0x6F12A5C0 provenance` round：
     - `runtimeModelCreateCount = 18`
     - `runtimeModelCreateCallerBuildChildLinksCount = 0`
     - `runtimeModelCreateCallerCreateSpriteRuntimeCount = 0`
     - `runtimeModelCreateCallerOtherCount = 18`
     - `runtimeCreationProvenanceCount = 18`
     - `lastRuntimeModelCreateCallerRva = 1221608 (0x12A3E8)`
     - `attachmentRigidChild/Owner/RootRuntimeCreateCallerKnownCount = 0 / 0 / 0`
     - `contractAttachmentRigidChild/Owner/RootRuntimeCreateCallerKnownCount = 0 / 0 / 0`
     - `attachmentRigidSample0 = (463646756 / 463646528 / 432740060)`
     - `attachmentRigidSample0*CreateCallerRva = 0 / 0 / 0`
   - `0x6F130D90 fallback init` round：
     - `runtimeModelInitCopyCount = 0`
     - `runtimeModelInitCopyPublishedFallbackCount = 0`
     - `lastRuntimeModelInitCallerRva = 0`
     - `attachmentRigidSample0 = (456306724 / 456306496 / 425400028)`
     - `attachmentRigidSample0*CreateCallerRva = 0 / 0 / 0`
   - 两轮共同保持：
     - `attachmentRigidCount = 3`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `runtimeChildLinkBuildCount = 0`
     - `attachmentAncestorIdentityHintWriteCount = 0`

#### 9.10.50.3 这轮新结论

1. `0x6F12A5C0` 这条 runtime create 路径现在已经不是“猜测”，而是 live + IDA 都确认了：
   - 在当前 `model_runtime_probe` 里它确实是热的
   - 当前热 create 全都来自 `ModelHandle_TryResolveRuntimeModel(0x6F12A3C0)` 这层 wrapper family
   - 不是来自：
     - `CModelComplex_BuildChildRuntimeModelLinks(0x6F131F60)`
     - `CreateSpriteRuntimeFromSourceObject(0x6F185250)`
2. 但当前 anonymous attachment trio 仍然**完全不在这条 hot create family 里**：
   - raw attachment 与 contract attachment 的 `root/owner/child runtime` 三层 create caller 仍全是 `0`
3. 补上 `0x6F130D90` fallback 之后，结果仍是：
   - `runtimeModelInitCopyCount = 0`
   - 说明当前场景下也没有出现“绕开 `0x6F12A5C0`、直接走 `CModelComplex_CopyFromModelData`”的 hot direct-init family
4. 因而当前 hard conclusion 再次收紧为：
   - 这批 anonymous attachment runtime trio 既不在当前 hot `0x6F12A3C0 -> 0x6F12A5C0` create family
   - 也不在 hot `0x6F130D90` direct-init fallback family
   - 当前 missing owner/source 更像来自：
     - 另一族 producer / constructor family
     - 或者一个发生在当前 hot-shadow 观察窗之前的更早创建阶段

#### 9.10.50.4 当前 blocker 与下一步

1. 当前 blocker 已更新为：
   - 不能再继续在
     - `0x6F12A3C0`
     - `0x6F12A5C0`
     - `0x6F130D90`
     - `0x6F131F60`
     这些已证伪/已定位的 hot create/init family 上继续兜圈
2. 下一轮固定主线：
   - 继续围绕最新 fresh sample runtime trio：
     - `rootRuntimeModelPtr = 456306724`
     - `ownerRuntimeModelPtr = 456306496`
     - `childRuntimeModelPtr = 425400028`
   - 直接去找“这棵 anonymous runtime tree 第一次进入 local-point/controller 语义面之前”的更早 producer/publisher
   - 优先方向不再是 attachment 末端 recover，而是：
     - 更早创建阶段
     - 或另一个不经过当前 hot `PromoteRuntimeModel / CopyFromModelData` 家族的 constructor/publisher

#### 9.10.51 2026-04-23 凌晨 Codex 继续把 probe 上抬到 `CModel/CModelComplex` constructor family，并确认 anonymous attachment trio 也不在当前 hot constructor 面里

#### 9.10.51.1 本轮代码

1. 在 `war3_model_hook.*` 里新增 constructor provenance probe：
   - `Hook_RuntimeModelPlainCtor(0x6F121880)`
   - `Hook_RuntimeModelComplexCtor(0x6F1219C0)`
   - 新增统计：
     - `runtimeModelCtorCount`
     - `runtimeModelComplexCtorCount`
     - `runtimeModelPlainCtorCount`
     - `runtimeModelCtorCallerPromoteCount`
     - `runtimeModelCtorCallerOtherCount`
     - `lastRuntimeModelCtorRuntimeModelPtr`
     - `lastRuntimeModelCtorCallerRva`
     - `lastRuntimeModelCtorKind`
2. constructor probe 直接复用 `ModelInstanceRegistry::noteRuntimeCreationProvenance(...)`：
   - 只发布 `runtimeModelPtr + ctor caller RVA`
   - 不再依赖 `LooksLikeRuntimeModelPtr(...)`，避免 ctor 初期字段尚未填完时被误过滤
3. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 新增 ctor counters / last sample 输出，确保 `model_runtime_probe` 能直接看到：
     - ctor 总量
     - plain/complex 分布
     - promote caller vs other caller 分布
     - 最新 ctor caller RVA
4. IDA 本轮命名/注释回写：
   - `0x6F121880 -> CModel_Ctor`
   - `0x6F1219C0 -> CModelComplex_Ctor`
   - `0x6F128140 -> HModelComplex_AllocWithOwnedModelDataHandle`
   - `0x6F128220 -> HModel_AllocWithOwnedModelDataHandle`
   - `0x6F12A400 -> CreateEmptyHModelWithOwnedModelData`
   - `0x6F1261D0` 补充了“在 complex/plain 分支之上选择并继续初始化”的注释

#### 9.10.51.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_ctor_family_20260423_0500.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_ctor_family_retry_20260423_0506.json`
3. 两轮共同关键输出
   - `runtimeModelCtorCount = 513`
   - `runtimeModelComplexCtorCount = 0`
   - `runtimeModelPlainCtorCount = 513`
   - `runtimeModelCtorCallerPromoteCount = 18`
   - `runtimeModelCtorCallerOtherCount = 495`
   - `lastRuntimeModelCtorCallerRva = 1222186 (0x12A62A)`
   - `lastRuntimeModelCtorKind = 1 (plain)`
   - `attachmentRigidCount = 3`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `attachmentRigidSample0Root/Owner/ChildRuntimeCreateCallerRva = 0 / 0 / 0`
4. 两轮运行态附加现象
   - 两次 `model_runtime_probe` 都在 `hot-shadow` 阶段返回 `wait_until stalled`
   - 但 control-plane 已稳定给出本轮需要的 ctor/attachment summary，因此这轮结论仍然有效

#### 9.10.51.3 这轮新结论

1. 当前 hot constructor 面已经不是空白：
   - plain `CModel_Ctor(0x6F121880)` 在 `model_runtime_probe` 下是热的
   - 而且并不只来自 `CModelData_PromoteToRuntimeModel(0x6F12A5C0)`：
     - `18` 次来自 promote family
     - `495` 次来自其它 plain-model constructor family
2. 但当前 anonymous attachment trio 仍然**完全不在这个 hot constructor 面里**：
   - raw attachment / contract attachment 的 `root/owner/child runtime` create caller 仍全部是 `0`
   - 说明它们不只是“不在 hot create/init family”
   - 现在连“当前 hot plain ctor family”也不在
3. 因而当前 hard conclusion 再次收紧为：
   - 这批 anonymous trio 更像发生在：
     - 当前 hot-shadow 观察窗之前的更早创建阶段
     - 或 hook 安装前的更早生命周期阶段
   - 而不是当前 ready/hot-frame 窗口内某个漏掉的 constructor wrapper

#### 9.10.51.4 当前 blocker 与下一步

1. 当前 blocker 更新为：
   - 不应再继续在
     - `0x6F121880`
     - `0x6F1219C0`
     - `0x6F128140`
     - `0x6F128220`
     - `0x6F12A400`
     这些当前 hot ctor family 上继续加更细的 blind probe
2. 下一轮固定主线：
   - 转去确认 anonymous runtime trio 是否创建于当前 model hook 安装之前
   - 优先方向：
     - 对齐 `war3_model_hook::Init(...)` 的真实安装时机
     - 对比 trio 的 first-seen / first-publish 时机
     - 必要时把 constructor provenance probe 前移到更早 bootstrap/lifecycle 阶段

#### 9.10.52 2026-04-23 清晨 Codex 已把 model hook 安装前移到 bootstrap，并首次命中了 anonymous attachment trio 的 owner-runtime provenance

#### 9.10.52.1 本轮代码

1. `d3d9_war3_hook.cpp`
   - 在 `War3Hook::InstallHooks(...)` 的 bootstrap 路径里提前执行：
     - `war3::model::Init(gameInfo.base);`
   - 这样 model provenance probe 不再等到：
     - `ActivateWar3Runtime(...)`
     - `InstallGameHooks(...)`
     之后才安装
2. 本轮没有改 semantic consumer，也没有改 attachment contract；仅验证“把 model hook 前移到 bootstrap 是否能恢复 provenance”

#### 9.10.52.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_bootstrap_model_init_20260423_0512.json`
3. 关键输出
   - `attachmentRigidCount = 3`
   - `attachmentRigidOwnerRuntimeCreateCallerKnownCount = 3`
   - `contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount = 3`
   - `attachmentRigidSample0OwnerRuntimeCreateCallerRva = 1221608 (0x12A3E8)`
   - `contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva = 1221608 (0x12A3E8)`
   - `attachmentRigidSample0RootRuntimeCreateCallerRva = 0`
   - `attachmentRigidSample0ChildRuntimeCreateCallerRva = 0`
   - `runtimeModelCtorCount = 1863`
   - `runtimeModelComplexCtorCount = 561`
   - `runtimeModelPlainCtorCount = 1302`
   - `runtimeModelCtorCallerPromoteCount = 685`
   - `runtimeModelCtorCallerOtherCount = 1178`
   - `runtimeModelCreateCount = 685`
   - `lastRuntimeModelCreateCallerRva = 1221608 (0x12A3E8)`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. 运行态现象
   - 本轮 `model_runtime_probe` 仍在 `hot-shadow` 阶段返回 `wait_until stalled`
   - 但 `shadowRuntimeSummary` 已给出稳定的新 provenance 证据，结论有效

#### 9.10.52.3 这轮新结论

1. “model hook 安装过晚”这条怀疑现在已经被部分坐实：
   - 只把安装时机前移到 bootstrap
   - anonymous attachment trio 的 `ownerRuntimeModelPtr` 就首次拿到了 create caller
   - caller 直接落在 `ModelHandle_TryResolveRuntimeModel(0x6F12A3C0)` 内的 `0x12A3E8`
2. 这说明：
   - 当前至少有一层 provenance 丢失，确实是因为以前 model hook 安装得太晚
   - owner runtime 并不是“永远匿名”，而是之前错过了它的 create/publish 时机
3. 但这轮同时也证明：
   - `rootRuntimeModelPtr` 仍然是 `0`
   - `childRuntimeModelPtr` 仍然是 `0`
   - `contractAttachmentRigidCountWithAnyIdentity` 仍然是 `0`
   - `semanticCoreAttachmentRigidResolved` 仍然是 `0`
4. 所以当前 hard conclusion 更新为：
   - late-install 确实是 blocker 的一部分，而且已经为 owner runtime 打开了突破口
   - 但当前真正缺的 object identity 仍未闭环
   - 下一步应从“已拿到 provenance 的 owner runtime”继续往 source/identity 发布层追，而不是再泛化地重挖 ctor 家族

#### 9.10.52.4 当前 blocker 与下一步

1. 当前 blocker 更新为：
   - owner runtime provenance 已恢复
   - 但 root/child runtime provenance 仍缺
   - 更关键的是，owner runtime 仍未转化成 `worldObjectEntry / sceneNode / unitPtr / jHandle / rawcode`
2. 下一轮固定主线：
   - 以已命中的 `ownerRuntimeModelPtr -> 0x12A3E8(ModelHandle_TryResolveRuntimeModel)` 为新的 authoritative 入口
   - 继续向上追 `handle/modelData/source object` 的 identity 发布层
   - 同时观察 root/child runtime 是否仍属于另一条未覆盖的更早发布链

#### 9.10.53 2026-04-23 下午 Codex 已验证：bootstrap child-link family 是热的，但当前 anonymous attachment sample child runtime 根本不在 runtime parent-link 图里

#### 9.10.53.1 本轮代码

1. `src/d3d9/war3/model/war3_model_hook.h/.cpp`
   - 新增只读查询：
     - `RuntimeParentLinkQueryResult`
     - `QueryRuntimeParentLink(void* childRuntimeModelPtr, ...)`
   - 目的不是改 attachment 逻辑，而是直接回答：
     - 当前 sample child runtime 是否真的被 `BuildChildRuntimeModelLinks` 链登记到了 `g_runtimeParentLinks`
2. `src/d3d9/war3/render/war3_shadow_runtime_bridge.h/.cpp`
   - 新增 attachment sample parent-link summary 字段：
     - `attachmentRigidChildRuntimeParentLinkKnownCount`
     - `contractAttachmentRigidChildRuntimeParentLinkKnownCount`
     - `attachmentRigidSample0/1ChildRuntimeParentRuntimeModelPtr`
     - `attachmentRigidSample0/1ChildRuntimeParentLinkSourceMeta`
     - `attachmentRigidSample0/1ChildRuntimeParentLinkLastSeenFrame`
     - contract mirror 字段
3. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - 将上述新字段接入 `get_shadow_runtime_summary`

#### 9.10.53.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_parent_link_20260423_134238.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_parent_link_retry_20260423_134414.json`
3. 两轮共同关键输出
   - `useIsolatedDesktop = true`
   - `windowed = true`
   - `attachmentRigidCount = 2`
   - `attachmentRigidChildRuntimeParentLinkKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeParentLinkKnownCount = 0`
   - `attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr = 0`
   - `attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr = 0`
   - `runtimeChildLinkBuildCount = 505`
   - `runtimeChildLinkBuiltChildCount = 2489`
   - `attachmentAncestorIdentityHintWriteCount = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. 运行态现象
   - 本轮两次 `model_runtime_probe` 仍停在 `stage=hot-shadow`
   - 但 control-plane summary 已稳定给出 parent-link 对账结论
   - launch metadata 同时证明：
     - 当前 probe 仍在隔离桌面运行
     - 且没有切全局全屏显示模式

#### 9.10.53.3 这轮新结论

1. `bootstrap child-link hook` 不是“没生效”：
   - `runtimeChildLinkBuildCount=505`
   - `runtimeChildLinkBuiltChildCount=2489`
   说明 `BuildChildRuntimeModelLinks` family 在当前 fresh run 里是真热的
2. 但当前 anonymous attachment sample child runtime：
   - `430902788`
   - `430903236`
   都没有命中任何 parent-link entry
3. 这意味着当前 blocker 已从：
   - “ancestor merge 没把已有 parent-link 身份合并下来”
   收紧成：
   - “当前 sample child runtime 根本不属于我们现在捕到的 child-link producer family”
4. 因此当前不该再继续在：
   - `MergeAttachmentHintsFromAncestorRuntimes(...)`
   - `ancestor identity repair`
   这层兜圈
5. 关于黑屏/雪花屏排查：
   - 当前 fresh probe 的 launch 元数据已证明：
     - `useIsolatedDesktop=true`
     - `windowed=true`
   - 所以这轮测试本身没有再去切前台显示模式
   - 更可能是更早一轮非当前 preset 的运行态或旧 hook 组合触发了显示异常，而不是本轮 parent-link probe 继续在切全屏

#### 9.10.53.4 当前 blocker 与下一步

1. 当前 blocker 更新为：
   - anonymous attachment 的 child runtime 不在当前 `BuildChildRuntimeModelLinks` parent-link 图里
   - 所以 object identity 丢失点仍在更早的 producer/construction family
2. 下一轮固定主线：
   - 继续围绕 sample trio：
     - `rootRuntimeModelPtr`
     - `ownerRuntimeModelPtr`
     - `childRuntimeModelPtr`
   - 向更早的 runtime create / producer 发布层上抬
   - 优先确认它们是否来自：
     - `ModelHandle_TryResolveRuntimeModel` 之外的另一族 runtime construction family
     - 或者 hook 安装前更早的一次 publish 窗口

#### 9.10.54 2026-04-23 下午 Codex 已验证：当前 anonymous attachment 已经进了 parent-link 图，但仍完全游离于 model/resource/pose/source-object registry

#### 9.10.54.1 本轮代码

1. `src/d3d9/war3/render/war3_shadow_runtime_bridge.h/.cpp`
   - 新增 raw/contract attachment sample 的 runtime-registry 对账字段：
     - `attachmentRigidChildRuntimeRecordKnownCount`
     - `attachmentRigidChildRuntimeModelResourceKnownCount`
     - `attachmentRigidChildRuntimePoseKnownCount`
     - contract mirror 字段
   - 新增 sample 级直观字段：
     - `attachmentRigidSample0/1ChildRuntimeCreateModelDataPtr`
     - `attachmentRigidSample0/1ChildRuntimeSourceObjectPtr`
     - `attachmentRigidSample0/1ChildRuntimeModelResourcePtr`
     - `attachmentRigidSample0/1ChildRuntimeModelKey`
     - `attachmentRigidSample0/1ChildRuntimePoseMatrixCount`
     - contract mirror 字段
2. `src/d3d9/war3/tools/war3_control_plane.cpp`
   - 将上述 runtime-registry 对账字段并入 `get_shadow_runtime_summary`
3. `src/d3d9/war3/model/war3_model_hook.cpp`
   - 在 `PublishRuntimeSourceObject(...)` 增加窄 fallback：
     - source publish 同步尝试 `sourceObject -> unit/jHandle/rawcode`
     - 若成功，则立刻 `noteRuntimeOwnerIdentity(...)`
   - 目的不是改 shadow 逻辑，而是直接回答：
     - 当前 anonymous attachment 是否只是“owner runtime 已有 source，但没有被回灌成 owner identity”
4. `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`
   - 在 `TryResolveRuntimeModelSemanticKey(...)` 增加 direct fallback：
     - `OwnedModelDataHandle -> resolveDirectModelResourcePtr(...)`
   - 目的不是改 renderer core，而是直接回答：
     - 当前匿名 runtime 是否只是没命中 `findRuntimeModelResource(...)`
     - 但其实还能从 runtime 本体 direct handle 路线解回 `modelResourcePtr`

#### 9.10.54.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮补丁后都通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_registry_20260423_145545.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_owner_identity_from_source_20260423_151209.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_direct_semantic_key_20260423_151823.json`
3. 所有运行共同关键输出
   - `useIsolatedDesktop = true`
   - `windowed = true`
   - 当前 probe 继续在隔离桌面窗口模式下运行，不会切前台全屏
4. `child_registry` 关键输出
   - `attachmentRigidCount = 3`
   - `attachmentRigidChildRuntimeRecordKnownCount = 0`
   - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `attachmentRigidChildRuntimePoseKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeRecordKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `contractAttachmentRigidChildRuntimePoseKnownCount = 0`
   - 但同一轮里：
     - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`
     - `contractAttachmentRigidChildRuntimeParentLinkKnownCount = 2`
     - `attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr = 459176232`
5. `owner_identity_from_source` 关键输出
   - `attachmentRigidCount = 2`
   - `attachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
   - `attachmentRigidRootRuntimeOwnerIdentityCount = 0`
   - `attachmentRigidChildRuntimeOwnerIdentityCount = 0`
   - `runtimeOwnerIdentityCount = 16`
   - `completeIdentityCount = 15`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `attachmentRigidSample0OwnerRuntimeSourceObjectPtr = 0`
6. `direct_semantic_key` 关键输出
   - `attachmentRigidCount = 2`
   - `attachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `attachmentRigidChildRuntimeRecordKnownCount = 0`
   - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `attachmentRigidChildRuntimePoseKnownCount = 0`
   - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`

#### 9.10.54.3 这轮新结论

1. 需要正式修正上一轮结论：
   - `9.10.53 / 33.20` 里“当前 anonymous child runtime 不在 parent-link 图里”已经不是当前最新事实
   - 最新 fresh 样本已经明确显示：
     - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`
     - `contractAttachmentRigidChildRuntimeParentLinkKnownCount = 2`
2. 但这并不代表 blocker 解了，反而把问题收得更准：
   - 当前 anonymous child runtime 已经进了 parent-link 图
   - 但它们仍然完全不在：
     - `ModelInstanceRegistry`
     - `ShadowModelResourceCache`
     - `PoseRegistry`
     - `source-object` 发布链
3. 这说明当前问题已经不该再表述为：
   - “ancestor merge 没生效”
   - 也不该再表述为：
   - “child runtime 根本不属于 parent-link family”
4. 更准确的新 blocker 是：
   - 当前 anonymous attachment 已经属于 parent-link family
   - 但这个 family 上游的 runtime publish 契约仍然没有进入现有 `model/resource/pose/source-object` registries
5. 两条本轮窄 fallback 都没有抬起 identity：
   - `PublishRuntimeSourceObject -> noteRuntimeOwnerIdentity` 没把当前匿名样本认回来
   - `OwnedModelDataHandle -> direct modelResourcePtr` 也没有把 unique semantic key repair 点亮
6. 所以当前 remaining miss family 已继续收敛成：
   - `parent-link family 已有`
   - `但 registry family 仍缺席`

#### 9.10.54.4 当前 blocker 与下一步

1. 当前 blocker 更新为：
   - 当前 anonymous attachment 已经在 parent-link 图里
   - 但 child/root/owner runtime 仍完全游离于：
     - `model/resource/pose/source-object` 四套 registry
2. 下一轮固定主线：
   - 不再继续在 `ancestor merge` 或 `sourceObject auto-publish` 这层 blind-fix
   - 直接围绕当前 fresh sample：
     - `childRuntimeModelPtr`
     - `parentRuntimeModelPtr`
     - `ownerRuntimeModelPtr`
     - `sourceMeta = 3 / 10`
   - 上抬去找它们所属的更早 builder/publisher family
3. 工程侧优先排查两条路：
   - `sourceMeta = 3 / 10` 分别对应哪条 child-link build/publish 来源
   - `child/root runtime` 本体是否还挂着未被当前 registry 吃掉的 direct handle/modelData 血缘

### 9.10.55 Child-link bootstrap round: direct runtime bootstrap and resource-side child-group pairing both miss the current anonymous attachment samples, proving the observed parent-link family is not the same as the hot `BuildChildRuntimeModelLinks` family (2026-04-23 16:14:56.252 +08:00)

#### 9.10.55.1 本轮代码

1. `src/d3d9/war3/model/war3_model_hook.cpp`
   - 先补齐 `BootstrapRuntimeModelResourceLineage(...)` 的前置声明，修复上一轮仅因声明顺序导致的编译失败；
   - 保留 `RecordObservedRuntimeChildLink(...)` 里的 direct runtime bootstrap：
     - 对 `parentRuntimeModelPtr`
     - 对 `childRuntimeModelPtr`
     - 都直接尝试 `OwnedModelDataHandle -> direct modelResource` 回灌
   - 随后新增资源侧 child-group 配对逻辑：
     - `ModelDataChildLinkProbeRecord`
     - `CollectModelDataChildRuntimeLinks(...)`
     - `BootstrapRuntimeChildLineageFromModelData(...)`
   - 该逻辑直接使用已存在的 `CModelDataOffsets::ChildRuntimeGroupRecords(0xC8)` 与 `ChildRuntimeGroupCount(0xD0)`：
     - 读取资源侧 child group
     - 读取资源侧 child link 节点 `+0x08 = child modelData`
     - 读取资源侧 child link 节点 `+0x0C = attach tag/sourceMeta`
     - 按 bucket reverse 后与 runtime bucket 做一一配对
     - 将配对结果回灌到：
       - `ModelInstanceRegistry::noteRuntimeCreationProvenance(...)`
       - `ShadowModelResourceCache::noteRuntimeModelBinding(...)`
2. 本轮目的不是直接点亮 renderer，而是直接回答两个问题：
   - 当前 anonymous attachment sample 是否只是“漏了 runtime direct bootstrap”？
   - 当前 anonymous attachment sample 是否其实属于当前 hot 的 `0x6F131F60 CModelComplex_BuildChildRuntimeModelLinks` build 家族，只是我们没把资源侧 child group 接进 registry？

#### 9.10.55.2 本轮验证

1. `ninja -C build32 -j1`
   - direct runtime bootstrap 编译修复后通过
   - child-group pairing 接入后再次通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_link_bootstrap_20260423_155157.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_child_group_bridge_20260423_160930.json`
3. 运行约束
   - 两轮 fresh run 都保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - 收尾检查 `war3_autotest.current_state`：
     - `war3Alive = false`
     - 没有残留前台/隔离桌面 War3 进程
4. `child_link_bootstrap` 关键输出
   - `attachmentRigidCount = 2`
   - `attachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `attachmentRigidChildRuntimeRecordKnownCount = 0`
   - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `attachmentRigidChildRuntimePoseKnownCount = 0`
   - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`
5. `child_group_bridge` 关键输出
   - `attachmentRigidCount = 2`
   - `attachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `attachmentRigidChildRuntimeRecordKnownCount = 0`
   - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `attachmentRigidChildRuntimePoseKnownCount = 0`
   - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`
   - sample 级仍然是：
     - `attachmentRigidSample0ChildRuntimeCreateModelDataPtr = 0`
     - `attachmentRigidSample0ChildRuntimeModelResourcePtr = 0`
     - `attachmentRigidSample1ChildRuntimeCreateModelDataPtr = 0`
     - `attachmentRigidSample1ChildRuntimeModelResourcePtr = 0`
6. 本轮最关键的 sample 对照
   - 当前 anonymous sample：
     - `attachmentRigidSample0RootRuntimeModelPtr = 466385192`
     - `attachmentRigidSample1RootRuntimeModelPtr = 466387124`
   - 当前 hot build hook 最新样本：
     - `lastRuntimeChildLinkBuildParentRuntimeModelPtr = 757641296`
     - `lastRuntimeChildLinkBuildModelDataPtr = 466388276`
     - `lastRuntimeChildLinkBuildChildRuntimeModelPtr = 0`
     - `lastRuntimeChildLinkBuildSourceMeta = 0`
   - 即：
     - 当前 anonymous sample 的 root runtime 与当前 hot `BuildChildRuntimeModelLinks` 样本根本不是同一棵 runtime
7. 额外仍成立的旧事实
   - 当前 anonymous sample 的 owner runtime 仍只有旧 provenance：
     - `OwnerRuntimeCreateCallerRva = 0x12A3E8`
     - `OwnerRuntimeCreateModelDataPtr != 0`
   - 但 sample root/child runtime 仍没有新的 `createModelData` 血缘

#### 9.10.55.3 本轮新结论

1. 需要正式排除这两条解释：
   - “当前 anonymous attachment sample 只是没走 runtime direct bootstrap”
   - “当前 anonymous attachment sample 其实就是当前 hot `0x6F131F60` build family 的 child，只是资源侧 child group 没有被接进 registry”
2. 这两条现在都已经被 live 证伪：
   - direct runtime bootstrap 无效
   - resource-side child-group pairing 无效
3. 当前更准确的新结论是：
   - anonymous attachment sample 的 parent-link 现在**确实可见**
   - 但它们不是当前 hot `BuildChildRuntimeModelLinks` 样本直接产出的那一批 child runtime
   - 也就是说：
     - 当前 hot-shadow summary 里这批 anonymous attachment sample 所属的 parent-link family
     - 与我们本轮在 `0x6F131F60` 上接到的 build family
     - 不是同一条 runtime publish 窗口
4. 因此 blocker 已继续收紧为：
   - 当前 anonymous sample 的 parent-link 很可能来自：
     - 更早的 build/publish 窗口（hook 安装前或 hot-shadow 观察窗前）
     - 或 `0x6F12EC90 / 0x6F12E900 / 0x6F182300` 这条 local-point/controller producer family 的更上游 caller
   - 而不是继续留在 `0x6F131F60` 本层 blind-fix 能解决

#### 9.10.55.4 当前 blocker 与下一步

1. 当前 blocker 更新为：
   - anonymous attachment sample 已有 parent-link
   - 但这条 parent-link family 不是当前 hot `BuildChildRuntimeModelLinks` build 样本直接产出的 family
   - 因而继续在 `0x6F131F60` 上补 registry/resource bootstrap 的收益已经显著下降
2. 下一轮固定主线：
   - 不再继续在 `0x6F131F60` blind-fix
   - 直接沿：
     - `0x6F12EC90`
     - `0x6F12E900`
     - `0x6F182300 / 0x6F1820C0`
   - 的 caller/producer family 上抬，查“是谁在当前 hot-shadow 可观测窗口里第一次同时持有 anonymous runtime tree 与 logical/source owner”
3. 工程侧优先排查两条路：
   - `RecordObservedRuntimeChildLink(...)` 当前命中的 root/child runtime family 是否来自更早 retained child tree，而不是本帧 freshly built tree
   - local-point/controller producer family 是否在 caller 层还带着：
     - root runtime 的 `OwnedModelDataHandle`
     - owner/source object
     - sprite / source context

#### 9.10.56 2026-04-23 已确认：当前 anonymous attachment family 的 owner runtime 会进入 sprite-frame full update，但进入时 `a3/context` 仍为空，因此主线继续锁定 `0x6F182300 / 0x6F1820C0` caller 链

1. 本轮代码
   - `war3_model_registry.h/.cpp`
     - 扩展 `AttachmentRigidRegistry` 的 runtime 反查：
       - `findByOwnerRuntimeModel(...)`
       - `findByRootRuntimeModel(...)`
       - `findByAnyRuntimeModel(...)`
   - `war3_model_hook.h/.cpp`
     - 新增 sprite-frame attachment hit 观测；
     - 先验证 anonymous attachment family 是否进入 sprite-frame 层；
     - 再补充 update-kind/context 观测，用于区分：
       - full update (`0x6F182300 / 0x6F1820C0`)
       - lite update
     - 新增输出：
       - `spriteFrameAttachmentContextHintCount`
       - `spriteFrameAttachmentFullUpdateHitCount`
       - `spriteFrameAttachmentLiteUpdateHitCount`
       - `lastSpriteFrameAttachmentContextPtr`
       - `lastSpriteFrameAttachmentUpdateKind`
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 同步 bridge summary 字段
   - `war3_control_plane.cpp`
     - 将上述新字段接入 `get_shadow_runtime_summary`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_attachment_20260423_171521.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_kind_20260423_174025.json`
3. 关键输出
   - 两轮 fresh run 都继续保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - 第一轮 `sprite_frame_attachment`：
     - `spriteFrameAttachmentRootRuntimeHitCount = 0`
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentChildRuntimeHitCount = 0`
     - `lastSpriteFrameAttachmentRoleMask = 2`
   - 第二轮 `sprite_frame_kind`：
     - `spriteFrameAttachmentRootRuntimeHitCount = 0`
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentChildRuntimeHitCount = 0`
     - `spriteFrameAttachmentContextHintCount = 0`
     - `spriteFrameAttachmentFullUpdateHitCount = 2`
     - `spriteFrameAttachmentLiteUpdateHitCount = 0`
     - `lastSpriteFrameAttachmentRoleMask = 2`
     - `lastSpriteFrameAttachmentUpdateKind = 1`
     - `lastSpriteFrameAttachmentContextPtr = 0`
   - 同时 current state 收尾为：
     - `war3Alive = false`
4. 本轮新结论
   - 当前 anonymous attachment family 并不是“完全不进 sprite-frame 层”；
   - 更准确地说：
     - 进入 sprite-frame 层的是 **owner runtime**
     - 不是 root runtime
     - 也不是 child runtime
   - 并且这个命中发生在：
     - `full update`
     - 不是 `lite update`
   - 但本轮又同时证明：
     - 这些 owner-runtime hit 进入 full update 时，`a3/context` 仍然为 `0`
     - `spriteFrameSourceHintCount = 3` 仍然存在
     - 但 `spriteFrameSourceResolvedIdentityCount = 0`
   - 因而现在最准确的 blocker 是：
     - anonymous attachment family **已经进入 `0x6F182300 / 0x6F1820C0` 这一层**
     - 但进入这层时，当前 hot-shadow 窗口里能看到的 `a3/context` 已经为空，无法直接给出 owner/source identity
5. 当前 blocker 与下一步
   - 不再回到 `lite update` 路线
   - 也不再回到 `0x6F131F60 BuildChildRuntimeModelLinks` 路线
   - 下一轮固定主线：
     - 继续沿 `0x6F182300 / 0x6F1820C0` 的 caller 链往上抬
     - 目标是找“同一条 owner runtime 在更上游哪一层仍然同时持有：
       - runtime/sprite
       - 非空 `a3/context`
       - logical/source owner”
   - 这意味着：
     - `0x6F182300 / 0x6F1820C0` 仍然是当前最优先的 reverse gate
     - `0x6F12EC90 / 0x6F12E900` 暂退为次级交叉验证线，而不是主线

#### 9.10.57 2026-04-23 已进一步确认：anonymous attachment owner-runtime hit 的稳定 caller 是 `0x184ED3`，落在 `0x184E50 AttachModelToPoint` family 内，因此下一轮主线从“泛化 sprite-frame caller 链”收紧为“直接沿 AttachModelToPoint family 上抬”

1. 本轮代码
   - `war3_model_hook.h/.cpp`
     - 新增 sprite-frame attachment update-kind/context 观测：
       - `spriteFrameAttachmentContextHintCount`
       - `spriteFrameAttachmentFullUpdateHitCount`
       - `spriteFrameAttachmentLiteUpdateHitCount`
       - `lastSpriteFrameAttachmentContextPtr`
       - `lastSpriteFrameAttachmentUpdateKind`
     - 再新增 caller 观测：
       - `spriteFrameAttachmentCallerKnownCount`
       - `spriteFrameAttachmentCallerChangedCount`
       - `lastSpriteFrameAttachmentCallerRva`
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 同步 bridge summary
   - `war3_control_plane.cpp`
     - 输出 update-kind/context/caller 字段
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_kind_20260423_174025.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_caller_20260423_180513.json`
3. 关键输出
   - 两轮都保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - `sprite_frame_kind`：
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentContextHintCount = 0`
     - `spriteFrameAttachmentFullUpdateHitCount = 2`
     - `spriteFrameAttachmentLiteUpdateHitCount = 0`
     - `lastSpriteFrameAttachmentUpdateKind = 1`
     - `lastSpriteFrameAttachmentContextPtr = 0`
   - `sprite_frame_caller`：
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentCallerKnownCount = 2`
     - `spriteFrameAttachmentCallerChangedCount = 0`
     - `lastSpriteFrameAttachmentCallerRva = 0x184ED3`
     - `lastSpriteFrameAttachmentRoleMask = 2`
   - 收尾：
     - `war3Alive = false`
4. 本轮新结论
   - 当前 anonymous attachment family 的 owner-runtime hit：
     - 不是 lite update
     - 而是 full update
     - 并且进入该层时 `a3/context` 仍为空
   - 同时 caller probe 又进一步坐实：
     - 这批 full-update owner-runtime hit 的 caller 并不是“多个不稳定 caller”
     - 而是稳定收敛到同一个 RVA：`0x184ED3`
     - 且 `CallerChangedCount = 0`
   - 由于 `0x184ED3` 落在 `0x184E50 AttachModelToPoint` family 内：
     - 当前 reverse gate 已不再只是“沿 `0x6F182300 / 0x6F1820C0` 的 caller 链上抬”
     - 而是更具体地变成：
       - 沿 `AttachModelToPoint` family 的更上游 producer/caller 往上抬
5. 当前 blocker 与下一步
   - blocker 继续收紧为：
     - anonymous attachment owner-runtime 已在 `AttachModelToPoint` family 内进入 full sprite-frame update
     - 但这一步时 `a3/context` 已经为空，owner identity 仍没闭环
   - 下一轮固定主线：
     - 以 `0x184ED3` 为精确锚点
     - 直接逆 `0x184E50 AttachModelToPoint` family 的更上游 caller / producer
     - 查“在 `0x184ED3` 之前哪一层最后一次仍然同时持有：
       - parent/owner runtime
       - child sprite/runtime
       - 非空 context
       - logical/source owner”

#### 9.10.58 2026-04-23 已打通 anonymous attachment 的 owner identity/source 发布：`AttachModelToPoint` 命中的 parent runtime 来自 `CAttachedEffect_Init`，并且 ownerWidget 身份现已回灌到 raw attachment registry 与 contract

1. 本轮代码
   - `war3_model_hook.h/.cpp`
     - 为 `Hook_AttachedEffectInit` 新增 TLS scope，保留：
       - `effectPtr`
       - `ownerWidgetPtr`
     - 在 `NoteSpriteFrameAttachmentRuntimeHit(...)` 中新增：
       - `MaybePublishAttachedEffectInitParentRuntimeOwnerIdentity(...)`
     - 发布条件固定为：
       - 当前命中的是 attachment owner runtime
       - 命中 runtime 等于当前 `AttachModelToPoint` scope 的 parent runtime
       - 同时仍处在 `CAttachedEffect_Init` scope 内
     - 发布内容固定为：
       - `PublishRuntimeSourceObject(parentRuntimeModelPtr, parentSpritePtr, ownerWidgetPtr, sourceSpriteObjectPtr)`
       - `noteInstanceIdentity(...)`
       - `bindRuntimeModelToSprite(...)`
       - `noteRuntimeOwnerIdentity(...)`
       - `RecordRuntimePaletteTree(...)`
     - 再新增 `attachedEffectInitParentRuntimeOwnerPublishCount` / `lastAttachedEffectInitParentRuntimeModelPtr` 统计。
   - `war3_model_registry.h/.cpp`
     - `AttachmentRigidRegistry` 新增 `noteRuntimeIdentity(...)`
     - 用于在 owner runtime 身份晚于 raw attachment record 发布时，按 `child/owner/root runtime` 回灌更新现有 attachment record 本体。
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 同步新增 summary 字段
   - `war3_control_plane.cpp`
     - 输出上述新增字段
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_scope_20260423_182640.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_owner_publish_registry_20260423_1846.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_owner_publish_registry_state_20260423_1847.json`
3. 关键输出
   - 所有 fresh run 继续保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - attach-scope probe 已坐实：
     - `spriteFrameAttachmentAttachScopeHitCount = 2`
     - `spriteFrameAttachmentAttachScopeOwnerHitCount = 2`
     - `spriteFrameAttachmentAttachScopeParentRuntimeMatchCount = 2`
     - `lastAttachScopeCallerRva = 0x6BB384`
     - `lastAttachScopeHitRuntimeModelPtr = lastAttachScopeParentRuntimeModelPtr`
   - 进一步确认：
     - `0x6BB384` 落在 `0x6BB2C0 CAttachedEffect_Init` family 内
     - 也就是当前 anonymous attachment sample0 的 owner runtime 正是 `CAttachedEffect_Init -> AttachModelToPoint` 这条链里的 parent runtime
   - owner publish patch 落地后的 fresh 结果：
     - `attachedEffectInitParentRuntimeOwnerPublishCount = 2`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 2`
     - `attachmentRigidCountWithAnyIdentity = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 2`
     - `attachmentRigidCountWithSourceObject = 2`
     - `contractAttachmentRigidCountWithSourceObject = 2`
     - `attachmentRigidSample0SourceObjectPtr = attachmentRigidSample0OwnerRuntimeSourceObjectPtr`
     - `lastAttachedEffectInitParentRuntimeModelPtr = lastAttachScopeParentRuntimeModelPtr`
   - 收尾状态：
     - `war3Alive = false`
4. 本轮新结论
   - 当前 anonymous attachment family 的“上层身份不完整”问题已经不再是 primary blocker：
     - `CAttachedEffect_Init` 确实是 parent/owner runtime 的上游 owner producer
     - 之前缺的是“ownerWidget 身份没有在 parent runtime 命中时及时发布”
   - 这一步现在已经闭环：
     - parent runtime 已拿到 `owner identity + sourceObject`
     - raw attachment registry 也已被回灌，不再只有 contract live repair 有身份
   - 因而当前 reverse gate 可正式视为通过：
     - `AttachModelToPoint / CAttachedEffect_Init` 这条 owner identity publish chain 已经打通
5. 当前 blocker 与下一步
   - 当前剩余 blocker 已切换为：
     - child runtime 的 `modelResource / pose` 仍未闭环
     - 证据：
       - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
       - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
       - `attachmentRigidChildRuntimePoseKnownCount = 0`
       - `contractAttachmentRigidChildRuntimePoseKnownCount = 0`
       - `semanticCoreAttachmentRigidResolved = 0`
       - `semanticCoreSkippedNoPoseAnonymousSubpart = 2`
   - 下一轮固定主线：
     - 不再回头补 owner/source identity
     - 直接转到 anonymous attachment child runtime 的 `geometry/resource/pose` 发布链
     - 目标是把 `semanticCoreAttachmentRigidResolved` 从 `0` 抬起来

#### 9.11.07 2026-04-24 Phase 3 局部推进：resource-owner world-pose rigid fallback 已让 semantic core 从 0 draw 变为稳定 2 draw，但 attachment/skinned 正式验收仍未过

1. 本轮代码
   - `war3_shadow_renderer_core.h/.cpp`
     - 新增 `attachmentRigidMatchByChildSpriteRuntimeGeoset` 统计，用于区分 internal child runtime 不命中但 `childSprite->Model` 命中 geoset 的情况。
     - `TryResolveAttachmentByChildRuntimeGeoset(...)` 增加 `childSprite->Model` alias 尝试；fresh 结果证明这条对当前样本不命中。
     - `TryAugmentRenderableSemanticRecovery(...)` 增加 resource-cache runtime-owner 回填：匿名 geoset 可从 `findRuntimeModelOwner(...)` 补上 `runtimeModelPtr / modelResourcePtr / modelKey`，不再直接停在 no-identity/anonymous-subpart。
     - pose 查询增加 `CModelComplex +0xA0` / `-0xA0` alias：resource ownership 可能在 CModel base，而 sprite-frame world pose 可能在 complex extension 侧。
     - 当 skinned resource 没有 matrix palette 但 resource-owner alias 有 sprite-frame world transform 时，走 semantic-only rigid fallback：使用上游 resource/geoset + world transform 产出 rigid draw packet，不触碰 VB/IB snapshot/freeze。
   - `war3_shadow_runtime_bridge.h/.cpp` 与 `war3_control_plane.cpp`
     - 输出 `semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset`。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_sprite_runtime_geoset_20260424_0233.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_resource_owner_pose_alias_20260424_0244.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_pose_snapshot_live_alias_20260424_0253.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_world_pose_rigid_fallback_20260424_0302.json`
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_world_pose_rigid_fallback_20260424_0307.json`
   - `AutoTest/artifacts/codex_low_pressure_static_reuse_world_pose_rigid_fallback_20260424_0312.json`
3. 关键输出
   - 所有 fresh run 均保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - `ninja -C build32 -j1` 通过。
   - 第一轮 child-sprite runtime geoset alias 证伪：
     - `semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset = 0`
     - `semanticCoreAttachmentRigidMatchMiss = 2`
   - resource-owner + pose alias 后，匿名子部件 blocker 从 anonymous 改为 pose lookup：
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 0`
     - `semanticCoreSkippedNoPoseLookupMiss = 2`
   - world-pose rigid fallback 后，三套场景均稳定：
     - `semanticCoreResolved = 2`
     - `semanticCoreRigidResolved = 2`
     - `semanticCoreDrawPacketCount = 2`
     - `semanticCoreSubmittedDrawCount = 2`
     - `semanticCoreSkippedNoPose = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 0`
     - `semanticCoreSkippedNoPoseLookupMiss = 0`
     - `objectFallbackDrawCount = 0`
   - 仍未抬起：
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkinnedResolved = 0`
     - `upperLayerEmitted = 0`
     - `upperLayerResolveAuthoritativeSkinned = 0`
     - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `attachmentRigidChildRuntimePoseKnownCount = 0`
4. 本轮新结论
   - 当前不再是“完全不能画”：semantic core 已经可以只靠上游 resource/geoset + sprite-frame world pose 产出 draw packet。
   - 当前 anonymous geoset 的 resource-owner runtime 与 sprite-frame source runtime 稳定呈现 `+0xA0` 关系：
     - 例：`lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr = 456264644`
     - `lastSpriteFrameSourceRuntimeModelPtr = 456264804`
   - 这说明 resource owner 和 pose/source 观察面分在 CModel base / CModelComplex extension 两侧；本轮 rigid fallback 解决了“世界位置可画”，但还没有拿到 skinned matrix palette。
5. 当前 blocker 与下一步
   - 当前 blocker 已从“上游身份不完整”推进到：
     - resource-owner/source-runtime family 只有 world transform 可消费，matrix palette 仍没有进入 `ShadowPoseStore`
     - attachment rigid record 本体仍没有 child modelResource/pose，因此 `semanticCoreAttachmentRigidResolved` 仍为 0
   - 下一轮固定主线：
     - 增加 `resourceRuntimeOwner ±0xA0` 的 control-plane diagnostics（worldTransform/matrixCount/source identity）
     - 在 producer 侧把 sprite-frame source runtime 的 pose/source 规范回灌到 CModel base owner
     - 找到这批 geoset 的 matrix palette 发布点后，把当前 rigid fallback 升级回 canonical skinned / attachment rigid path

#### 9.11.08 2026-04-24 Phase 3 继续推进：resource-owner `+0xA0` source/context 已证伪为逻辑 owner，sprite-frame source 弱身份发布已降级为 observe

1. 本轮代码
   - `war3_model_hook.*`
     - 新增 sprite-frame source object 字段诊断：记录 `sourceObject+0/+0x20/+0x28`，并扫描 `0x00..0x100` 内 runtime/registry 候选。
     - 新增 `HasLogicalObjectIdentity(...)`，把 sprite-frame source identity publish 收紧为必须有 `unitPtr / jHandle / rawcode` 之一。
     - 对只有 worldObjectEntry/sceneNode 的 source-frame 弱命中改为 observe，不再发布 runtime owner identity，避免把矩阵/context block 误当逻辑对象。
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - 输出 `spriteFrameSourceDeepIdentityResolvedCount`、`spriteFrameSourceObjectRuntimeFieldCandidateCount`、`spriteFrameSourceObjectRegistryFieldHitCount`。
     - 输出 last source-object 字段候选与最终 sprite-frame source identity 字段。
   - IDA 已命名并注释：
     - `0x6F0CB110 = WorldObjectList_AddEntry_ObjectSelfOwnerHint`
     - `0x6F0CB4A0 = WorldObjectList_FilterAndAppendVisibleCandidate`
     - `0x6F0CAAE0 = WorldObjectList_QueryVisibleCandidates_OptionalGate`
     - `0x6F0CAA90 = WorldObjectList_QueryVisibleCandidates_ForcedGate`
     - `0x6F0CB000 = WorldObjectList_TestEntriesAgainstSpriteBounds`
     - `0x6F184E50 = CSprite_AttachModelToPoint_DispatchUpdate`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_source_object_deep_probe_20260424_0330.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_deep_identity_merge_20260424_0338.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_source_field_merge_20260424_0348.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_source_logical_gate_20260424_0400.json`
3. 关键输出
   - 所有运行均为 `useIsolatedDesktop=true` / `windowed=true`，收尾无残留 War3 进程。
   - `ninja -C build32 -j1` 通过。
   - 最终 fresh run：
     - `semanticCoreRigidResolved = 2`
     - `semanticCoreDrawPacketCount = 2`
     - `semanticCoreSubmittedDrawCount = 2`
     - `objectFallbackDrawCount = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreAttachmentRigidPoseMissNoRecord = 2`
     - `spriteFrameSourceResolvedIdentityCount = 0`
     - `spriteFrameSourceObjectRuntimeFieldCandidateCount = 102`
     - `spriteFrameSourceObjectRegistryFieldHitCount = 96`
     - `lastSpriteFrameSourceObjectRuntimeFieldOffset = 0x100`
     - `lastSpriteFrameSourceObjectRegistryFieldOffset = 0x100`
     - `lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform = 1`
     - `lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount = 0`
4. 本轮新结论
   - `resourceRuntimeOwner + 0xA0` 仍然稳定提供 world transform，但没有 matrix palette。
   - `resourceRuntimeOwner + 0xA0` 发布的 sprite-frame source 参数不是逻辑对象；它更像 pose/source context block。
   - `sourceObject+0x100` 虽然能扫到 runtime/registry 候选，但把它合并进 owner 后得到的 world/scene 值是 context/matrix 风格值，不是可靠逻辑身份；该路线已证伪并回退为 observe-only。
   - AutoTest 的 `hot-shadow` 标红是 gate 要求 `unitCount>=1` / `semanticCoreSkinnedResolved>=1`，不是崩溃；当前 probe 场景依然只有 2 个 unknown rigid renderable。
5. 当前 blocker 与下一步
   - 当前 blocker 已进一步收紧为：
     - 这 2 个 resource-owner renderable 可以 rigid fallback 绘制，但它们不是 attachment rigid record 本体。
     - 当前 sprite-frame source/context 无法提供 `unitPtr / jHandle / rawcode`；不能作为 authoritative owner producer。
     - matrix palette 仍未进入 `ShadowPoseStore`，所以 skinned / attachment rigid canonical path 仍未闭环。
   - 下一轮固定主线：
     - 不再把 sprite-frame `a3/sourceObject` 当 owner。
     - 直接上抬到调用 `CSpriteUber_PreRenderAndUpdatePosePalette_*` 的 owner/sprite producer，找第一次同时持有 logical object + sprite/runtime 的发布点。
     - 继续追 `0x6F12F7E0` 之后的 matrix palette 发布面，目标把当前 2 个 rigid fallback packet 升级为 skinned packet。

#### 9.11.09 2026-04-24 Phase 3 继续推进：resource-owner rigid 已正式分类；attachment child lineage 卡在 runtime child 到 modelData child 的映射

1. 本轮代码
   - `war3_shadow_renderer_core.*` 新增 `explicitResourceOwnerRigid*` resolve counters，当前 resource-owner world-pose fallback 不再混在 attachment miss 里。
   - `war3_shadow_runtime_bridge.*`、`war3_control_plane.cpp`、`war3_diagnostics_hub.*`、`war3_perf_monitor.cpp` 输出 explicit resource-owner rigid counters。
   - `AutoTest/war3_autotest_mcp.py` 修正 `model_runtime_probe` 的 hot-shadow gate，允许 semantic rigid-only observe 阶段用 `semanticRigidOnlyAccepted=true` 表示阶段性通过，不放宽最终 `dynamic_shadow_pressure` visual gate。
   - `war3_model_hook.*` 新增 child-lineage bootstrap counters，并尝试 runtime bucket ordinal、root link + owner modelData cross bootstrap、observed root links bootstrap。
   - IDA 已命名并注释 `0x6F12F7E0 = CModel_EvalPoseStackAndChildren`、`0x6F12EC90 = CModel_RecurseChildPoseStack`、`0x6F12FDC0 = CModel_CopyPoseMatrixRangeFromStack`、`0x6F12FF50 = CModel_FlushCurrentPoseStackToMatrices`。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_explicit_resource_owner_rigid_20260424_0455.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_child_lineage_bootstrap_diag_20260424_0520.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_child_lineage_observed_links_20260424_0550.json`
3. 关键输出
   - 所有 runtime 测试均为 `useIsolatedDesktop=true` / `windowed=true`，并使用 `avoid_foreground_switch=true` 收尾。
   - `ninja -C build32 -j1` 通过；`python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 最新 fresh run：`semanticCoreExplicitResourceOwnerRigidResolved=2`、`semanticCoreSubmittedDrawCount=2`、`objectFallbackDrawCount=0`、`semanticCoreAttachmentRigidResolved=0`、`semanticCoreSkinnedResolved=0`。
   - child-lineage：`attachmentChildLineageBootstrapAttemptCount=4`、`attachmentChildLineageBootstrapSuccessCount=0`、`attachmentChildLineageBootstrapMissNoModelDataLinksCount=0`、`attachmentChildLineageBootstrapMissNoUniqueChildCount=4`。
4. 本轮新结论
   - 当前 2 个 semantic draw packet 已正式确认是 resource-owner explicit rigid path，不是 attachment child record 本体。
   - attachment child 的 parent-link/identity 仍然存在，且 parent modelData host 能解析；失败点不是 no modelData links。
   - 新 blocker 是 runtime child 到 modelData child 的准确映射还没闭环：`bucket/sourceMeta`、全父模型唯一 child、runtime bucket ordinal、root link + owner modelData cross bootstrap、observed root links bootstrap 都未命中。
   - `0x6F12F7E0` 链已确认是 pose stack / child local transform 递归链；`0x6F12FDC0` 和 `0x6F12FF50` 是 final matrix array copy/flush 的 palette publisher 候选，仍需 live hook/counter 验证。
5. 当前 blocker 与下一步
   - 距离 canonical attachment rigid/skinned 还差 child runtime -> child modelData/modelResource 的准确映射，以及 child/root pose 或 explicit rigid transform 合法化。
   - 下一轮只追 `0x6F131F60` 中 `v9[2] = sub_6F12A5C0(v7[2])` 这条 build-time producer，直接记录 `childRuntimeModelPtr -> v7[2] childModelDataPtr -> direct modelResourcePtr`。
   - 如果 build-time 仍拿不到当前 attachment child，再转向 hook `0x6F12FDC0 / 0x6F12FF50` 验证 final matrix array copy/flush 是否能发布 child pose。

#### 9.11.10 2026-04-24 Phase 3 -> Phase 4 前半：attachment rigid 已进入 ShadowRendererCore semantic draw path

1. 本轮代码
   - `war3_model_registry.*`
     - 新增 `AttachmentRigidRegistry::promoteAttachmentChildRuntime(...)`，用 `AttachModelToPoint` 真实 child runtime 替换 earlier local-point/virtual child，同时保留 owner/root/localPoint/identity。
   - `war3_model_hook.*`
     - `RecordAttachModelToPointOwnerBinding(...)` 记录 attach point index，并在 parent runtime 命中时调用 promotion；新增 promotion counters 与 last promoted child/resource 样本。
     - `0x6F131F60 = CModelComplex_BuildChildRuntimeLinks_FromModelData` 继续保留 pre/post child modelData scan 诊断；高地址 list head 已证明不能用 signed-positive 判断。
   - `war3_shadow_runtime_contract.cpp`
     - `PopulateAttachmentChildSemanticKey(...)` 解析到 child modelResource 后，确保 `ShadowModelResourceCache` 有 child model resource / runtime model binding。
     - 若 attachment 解析导致 resource cache revision 变化，同一帧立即重建 `ShadowModelResourceStore`，避免 contract 先构建 resources、后补 attachment resource 导致 core 读不到。
   - `war3_shadow_renderer_core.*`
     - 在 manifest renderable 处理完后新增 attachment rigid supplemental path：直接从 `ShadowAttachmentRigidStore + ShadowModelResourceStore + ShadowPoseStore` 生成 rigid draw packet。
     - supplemental path 先按 `childRuntimeModelPtr + geosetIndex` / `childModelResourcePtr + geosetIndex` alias 查资源，再 fallback 到 resource records 扫描；使用现有 `resolveRecord` 与 draw dedup，不读 snapshot/freeze/VB/IB。
     - 修正 attachment rigid 成功时的统计口径：同步增加 `semanticCoreResolved` / `semanticCoreRigidResolved`。
     - validation runtime 的 freshness gate 增加 attachment supplemental candidate 判断，允许同一 manifest frame 在 attachment resource/pose 后补齐后重建一次。
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - 新增 supplemental counters：`semanticCoreAttachmentRigidSupplementalAttachmentCount`、`semanticCoreAttachmentRigidSupplementalResourceCandidateCount`、`semanticCoreAttachmentRigidSupplementalResolvedCount`、`semanticCoreAttachmentRigidSupplementalResourceMissCount`。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_build_time_modeldata_prescan_highptr_20260424_0650.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_promoted_child_runtime_20260424_0710.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attachment_rebuild_freshness_20260424_0850.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attachment_stats_fixed_20260424_0905.json`
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_attachment_rigid_smoke_20260424_0915.json`
   - `AutoTest/artifacts/codex_low_pressure_static_reuse_attachment_rigid_smoke_20260424_0925.json`
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_strict_gate_contrast_20260424_0940.json`
3. 关键输出
   - 全部 War3 runtime 验证均为 `useIsolatedDesktop=true` + `windowed=true`，收尾无残留 War3 进程。
   - `ninja -C build32 -j1` 通过。
   - `model_runtime_probe` 最新阶段 gate：
     - `semanticCoreConsidered = 98`
     - `semanticCoreResolved = 98`
     - `semanticCoreRigidResolved = 98`
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreAttachmentRigidSupplementalAttachmentCount = 1`
     - `semanticCoreAttachmentRigidSupplementalResourceCandidateCount = 96`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount = 96`
     - `semanticCoreSubmittedDrawCount = 98`
     - `objectFallbackDrawCount = 0`
   - `dynamic_shadow_pressure` semantic smoke：
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreSubmittedDrawCount = 98`
     - `semanticCoreSkippedNoIdentity = 0`
     - `semanticCoreSkippedNoPose = 0`
     - `objectFallbackDrawCount = 0`
   - `low_pressure_static_reuse` semantic smoke：
     - `semanticCoreResolved = 4`
     - `semanticCoreExplicitResourceOwnerRigidResolved = 4`
     - `semanticCoreSubmittedDrawCount = 4`
     - `objectFallbackDrawCount = 0`
   - strict `dynamic_shadow_pressure` gate 对照仍失败在 `hot-shadow`：
     - returned summary 仍是 pre-rebuild `semanticCoreSubmittedDrawCount=2`
     - `semanticCoreSkinnedResolved=0`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `objectFallbackDrawCount=0`
     - 该结果用于标记“正式 skinned/native gate 未通过”，不是 attachment rigid smoke 回归。
4. 本轮新结论
   - `0x6F131F60` 的当前 hook 层仍不能直接读出可用 child modelData list：pre/post link count 为 0，但 unreadable/high list head 计数很高；该层不能继续作为唯一 child resource producer。
   - `AttachModelToPoint` 真实 child runtime promotion 是有效路线：child resource/identity 可通过 contract 抬起。
   - 当前已经不是“上游数据层拿不到完整数据”：attachment owner、child resource、父级 world pose、core 消费链已闭环，semantic core 可以不靠旧 fallback 生成 attachment rigid draw packets。
   - 当前仍未完成的是 skinned canonical path / upper-layer authoritative skinned：`semanticCoreSkinnedResolved=0`、`upperLayerEmitted=0`。
   - 96 个 supplemental candidate 来自一个 attachment resource 的多 geoset rigid 展开；这是可用但需要继续做 draw/geometry cache 压力验证与可能的 geoset 可见性收窄。
5. 当前 blocker 与下一步
   - Phase 3 的 Runtime gate 已过：`semanticCoreAttachmentRigidResolved > 0`，且 fallback 为 0。
   - 下一步进入 Phase 4 correctness/perf 收口：
     - 用正式 `dynamic_shadow_pressure` gate 区分“attachment rigid 已可用”与“skinned 尚未完成”，不要再把 skinned=0 当作数据层失败。
     - 继续追 matrix palette / skinned canonical path，把 `semanticCoreSkinnedResolved` 与 `upperLayerEmitted` 抬起。
     - 对 96 个 attachment rigid draw 做 cache/dedup 观察，若 CPU 抬升，优先收窄 supplemental geoset 或做 geometry/palette build dedupe，不允许回退 snapshot/freeze。

#### 9.11.11 2026-04-24 Phase 4 前半：AutoTest hot-shadow gate 已能区分 attachment rigid 已通与 skinned 未完成

1. 本轮代码
   - `AutoTest/war3_autotest_mcp.py`
     - 阴影场景 preset 默认改为 `windowed=true`，继续保留 `useIsolatedDesktop=true`，避免全屏测试导致前台黑屏/雪花屏残留。
     - `wait_for_hot_shadow_frame(...)` 新增 summary-poll 模式：对 `dynamic_shadow_pressure` / `model_runtime_probe` 用低频 `get_shadow_runtime_summary(refreshSemanticFrameIfStale=true)` 等待 semantic summary，而不是先走会在隔离桌面同帧状态下 `stalled` 的高频 `wait_until` hot gate。
     - 新增阶段分类字段：`semanticAttachmentRigidOnlyAccepted`、`semanticAttachmentRigidGateSatisfied`、`semanticShadowPhase=attachment-rigid-ok-skinned-pending`。
     - `model_runtime_probe` 不再在 2 个 explicit rigid packet 时提前接受；现在优先等 `semanticCoreAttachmentRigidResolved > 0`。
     - `dynamic_shadow_pressure` strict gate 仍保持 `ok=false`，但 stage 改为 `hot-shadow-skinned-pending`，用于明确“attachment rigid semantic 已工作，skinned/native 签收未完成”。
2. fresh 工件
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_ready_poll_attachment_20260424_0718.json`
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_summary_only_gate_20260424_0805.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attachment_gate_after_autotest_fix_20260424_0818.json`
3. 关键输出
   - 全部 War3 runtime 测试均为 `useIsolatedDesktop=true` + `windowed=true`，收尾无残留 War3 进程。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `ninja -C build32 -j1` 通过。
   - `dynamic_shadow_pressure` strict gate 现在返回：
     - `stage = hot-shadow-skinned-pending`
     - `semanticShadowPhase = attachment-rigid-ok-skinned-pending`
     - `semanticCoreResolved = 98`
     - `semanticCoreRigidResolved = 98`
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount = 96`
     - `semanticCoreSubmittedDrawCount = 98`
     - `semanticCoreSkinnedResolved = 0`
     - `upperLayerEmitted = 0`
     - `nativeD3D9BackendExecutedDrawCount = 0`
     - `objectFallbackDrawCount = 0`
   - `model_runtime_probe` hot gate 现在等到 attachment rigid：
     - `hotOk = true`
     - `semanticAttachmentRigidOnlyAccepted = true`
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreSubmittedDrawCount = 98`
     - `objectFallbackDrawCount = 0`
4. 本轮新结论
   - 之前 strict gate 的 `hot-shadow` 失败不是 attachment runtime 回归；它是 AutoTest gate 在隔离桌面窗口化下过早使用 `wait_until` hot-frame 条件，停在 pre-attachment summary。
   - 手写 ready 后低频 summary 轮询和新的 summary-poll gate 都能稳定等到 `attachmentRigidResolved=96`。
   - 当前正式 blocker 更明确：attachment rigid semantic contract 已通过，剩余是 canonical skinned / upper-layer emission / native backend execution 未签收。
5. 当前 blocker 与下一步
   - 下一轮继续追 `semanticCoreSkinnedResolved=0` 与 `upperLayerEmitted=0`：
     - 优先查 skinned manifest/resource/pose/palette gate 为何不进入 authoritative skinned。
     - 不重开 `WorldObjectList+0x14`、`a3/sourceObject`、`sourceObject+0x100`、`childSprite->Model alias`。
     - 继续对 96 个 attachment supplemental draw 做 cache/dedup 与 geoset 可见性观察；若 CPU 抬升，只优化 semantic build/cache，不回退 snapshot/freeze/VB/IB。

#### 9.11.12 2026-04-24 Phase 4 继续收窄：skinned=0 是 root/unit manifest 缺口，不是 group-palette 算法失败

1. 本轮代码
   - `war3_shadow_renderer_core.*`
     - 新增 semantic core skinned candidate 诊断计数：
       - `semanticCoreRigidCandidateCount`
       - `semanticCoreSkinnedCandidateCount`
       - `semanticCoreSkinnedCandidatePoseReadyCount`
       - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount`
       - `semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount`
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - 将上述计数接入 `get_shadow_runtime_summary`。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_skinned_candidate_counters_20260424_0725.json`
3. 关键输出
   - `ninja -C build32 -j1` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 本轮 War3 runtime 测试继续为 `useIsolatedDesktop=true` + `windowed=true`，收尾无残留 War3 进程。
   - `model_runtime_probe` 最新 hot summary：
     - `semanticCoreResolved = 98`
     - `semanticCoreRigidResolved = 98`
     - `semanticCoreRigidCandidateCount = 0`
     - `semanticCoreSkinnedCandidateCount = 98`
     - `semanticCoreSkinnedCandidatePoseReadyCount = 0`
     - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 0`
     - `semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount = 96`
     - `semanticCoreSkinnedResolved = 0`
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreSubmittedDrawCount = 98`
     - `matrixPaletteCount = 44`
     - `shadowRuntimeModelCount = 226`
     - `shadowReadyGeosetCount = 3527`
     - `objectFallbackDrawCount = 0`
4. 本轮新结论
   - 当前不是没有 skinned resource：98 个 semantic candidate 都来自带 skinning data 的 resource。
   - 当前也不是 runtime 总体没有 pose/palette：`matrixPaletteCount=44`、`shadowRuntimeModelCount=226` 仍存在。
   - 真正问题是这些 visible/attachment candidate 没拿到 canonical pose palette；96 个已被 attachment rigid 路线按 rigid transform contract 合法接走，剩下 2 个走 explicit resource-owner rigid。
   - 所以 `semanticCoreSkinnedResolved=0` 不能继续解释为“group palette 解析算法失败”；更准确的 blocker 是 root/unit skinned renderable 没进入当前 manifest/semantic scene submission。
5. 当前 blocker 与下一步
   - 下一轮主线切到 root/unit skinned manifest 发布：
     - 查为什么当前热帧 manifest 只有 unknown/attachment/resource-owner 记录，`unitCount=0`、`recordsWithRuntimeModel=0`、`recordsWithModelResource=0`。
     - 查 `semanticSceneSubmitted=0` 是否只是 BeforeUi semantic scene 没执行，还是 unit-like capture 被 bypass 后没有等价 manifest record。
   - 目标不是改 attachment rigid；目标是让真正 root/unit skinned renderable 带 `runtimeModelPtr + modelResourcePtr + pose palette` 进入 `ShadowFrameManifest`，再让 `semanticCoreSkinnedResolved` 抬起。

#### 9.11.13 2026-04-24 Phase 4 root/unit manifest 补发布：skinned path 首次抬起

1. 本轮代码
   - `war3_shadow_runtime_contract.*`
     - 在 contract bundle 层新增 root/unit supplemental manifest records。
     - 种子只来自已可信的现有链路：`AttachmentRigid` owner/root runtime、`ModelInstanceRegistry` unit runtime、`ShadowObjectRegistry` Unit runtime。
     - 每轮最多补 96 条 root/unit record，并写入 `runtimeModelPtr + modelResourcePtr + runtimeGeoset/runtimeGeosetData + geosetIndex`，不走 VB/IB snapshot/freeze。
     - 新增 root/unit supplement counters，用于区分 no identity / attachment child / no pose / no resource / no geoset / duplicate / appended。
   - `war3_shadow_runtime_bridge.cpp`
     - `QueryShadowRuntimeBridgeSummary(refreshSemanticFrameIfStale=true)` 改为使用 supplemented bundle 判断 manifest freshness。
     - 隔离桌面尾帧若 build 已 `inProgress`，允许 control-plane 低频 observe-step 继续推进，避免 pending/in-progress 永久卡住。
   - `war3_control_plane.*`
     - `get_frame_manifest_summary` 改为读取 supplemented bundle，并输出 root/unit supplement counters。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_root_unit_supplement_20260424_0919.json`
3. 关键输出
   - `ninja -C build32 -j1` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 所有 War3 runtime 验证继续为 `useIsolatedDesktop=true + windowed=true`，并已停止残留 War3 进程。
   - `get_frame_manifest_summary` 最佳样本：
     - `recordsWithResolvedGeoset = 98`
     - `recordsWithRuntimeModel = 96`
     - `recordsWithModelResource = 96`
     - `unitCount = 96`
     - `rootUnitSupplementAppended = 96`
     - `rootUnitSupplementSkippedNoIdentity = 0`
     - `rootUnitSupplementSkippedNoResource = 0`
   - `get_shadow_runtime_summary` 最佳已完成 build 样本：
     - `semanticCoreSourceUnitCount = 61`
     - `semanticCoreSkinnedResolved = 31`
     - `semanticCoreSkinnedCandidatePoseReadyCount = 59`
     - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 31`
     - `semanticCoreAttachmentRigidResolved = 213`
     - `semanticCoreSubmittedDrawCount = 39`
     - `semanticCoreSkippedNoRuntimeGroupPalette = 0`
     - `objectFallbackDrawCount = 0`
     - `upperLayerEmitted = 0`
4. 本轮新结论
   - root/unit manifest 缺口已经实质突破：manifest 不再是 `unitCount=0 / runtimeModel=0 / modelResource=0`。
   - canonical skinned path 也已实质突破：`semanticCoreSkinnedResolved` 从 0 抬到 31，且 `poseReady/paletteReady` 同步抬起，说明当前已不是“上游数据完全不够”。
   - 仍未完成的是 freshness/perf 收口：supplemented manifest + attachment supplemental workload 会扩成较大的 semantic build，隔离桌面 control-plane observe 下 `semanticCoreBuildInProgress=true` 持续较久，最新 96 条 root/unit 补记录尚未完全 fresh。
5. 当前 blocker 与下一步
   - 下一轮不再重开 owner/source 路线；主线切为 semantic build 去重/限流：
     - 限制 attachment supplemental 与 root/unit supplemental 重复进入同一 build。
     - 优先让 build 先完成 root/unit skinned records，再处理 attachment diagnostic 扩展。
     - 目标是让 `semanticCoreFrameFresh=true`、`semanticCoreSourceUnitCount` 接近 96，同时保持 `semanticCoreSkinnedResolved > 0`、`objectFallbackDrawCount=0`。
   - `upperLayerEmitted=0` 仍未签收；待 build freshness 和 packet 数量稳定后再接 emission/visual gate。

#### 9.11.14 2026-04-24 Phase 4 freshness/perf 收口：attachment supplemental 阶段感知限流

1. 本轮代码
   - `war3_shadow_renderer_core.cpp`
     - attachment supplemental 每帧解析上限调整为阶段感知：
       - manifest 还没有 root/unit semantic records 时，上限保持 128，用于保留 attachment-only gate。
       - manifest 已有 root/unit semantic records 时，上限降为 16，让 root/unit skinned build 优先完成。
     - child runtime geoset probing 从 512 收窄为 128，降低尾帧 probe 成本。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_semantic_build_limited_20260424_0930.json`
3. 关键输出
   - `ninja -C build32 -j1` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 测试继续 `useIsolatedDesktop=true + windowed=true`，收尾无残留 War3 进程。
   - 限流前观察：
     - `buildRecordCount = 603`
     - `semanticCoreSkinnedResolved = 31`
     - `semanticCoreAttachmentRigidResolved = 213`
   - 限流后观察：
     - `buildRecordCount` 降为 `26`，随后 root/unit build 为 `98`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount = 16`
     - `semanticCoreSkinnedResolved = 4`
     - `semanticCoreSkinnedCandidatePoseReadyCount = 4`
     - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 4`
     - `semanticCoreAttachmentRigidResolved = 36`
     - `semanticCoreSubmittedDrawCount = 42`
     - `objectFallbackDrawCount = 0`
4. 本轮新结论
   - 限流方向正确：attachment supplemental 不再把 semantic build 扩到 603 条，control-plane 响应从多秒/超时明显下降。
   - skinned path 不是偶然一次性命中；限流后仍能重新抬到 `semanticCoreSkinnedResolved=4`。
   - 当前仍未完成的是 build ordering/freshness：root/unit records 已经在 manifest 中，但 control-plane observe 仍逐 chunk 推进，`semanticCoreFrameFresh` 还没稳定为 true。
5. 当前 blocker 与下一步
   - 下一轮固定只做 build ordering / bounded multi-chunk observe：
     - 对 `buildRecordCount <= 128` 的 root/unit build，允许 control-plane refresh 一次推进多个 chunk。
     - 或在 buildFrameChunk 中优先处理 root/unit records，避免先处理低价值 attachment/unknown 记录。
   - 不再调整 owner/resource/pose 理论；当前数据链已经够用，问题是 build 调度和最终 emission。

#### 9.11.15 2026-04-24 Phase 4 freshness 收口：root/unit 优先 build 与小闭环签收

1. 本轮代码
   - `war3_shadow_runtime_bridge.cpp`
     - control-plane refresh 的 observe completion 从 16 条记录扩到 128 条记录。
     - extra chunks 从 2 提到 8，并加 6000ms wall-clock budget，避免隔离桌面尾帧 build 永久 pending。
   - `war3_shadow_renderer_core.cpp`
     - root/unit supplemental renderable 的 runtime tree rescue 改为有界扫描，避免每条补记录扫完整 child tree。
   - `war3_shadow_runtime_contract.cpp`
     - root/unit semantic records 在 supplemented manifest 内稳定前置，避免先被旧 unknown/diagnostic records 卡住。
     - root/unit supplement 暂收为小闭环：每帧最多 16 条、每 runtime 最多 2 个 geoset。这个是 Phase 4 freshness gate 用的限流，不是最终全量覆盖策略。
   - `d3d9_device.cpp`
     - semantic scene bootstrap catchup 不再只处理“空 frame”；当已有非空 frame 但 source publish revision 落后时，也会推进 catchup。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_bounded_observe_samples_20260424_0940.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_bounded_tree_samples_20260424_0945.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_root_first_samples_20260424_0950.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_root_cap32_samples_20260424_0953.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_root_cap16_samples_20260424_0956.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_scene_catchup_samples_20260424_1002.json`
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_scene_catchup_smoke_20260424_1005.json`
3. 关键输出
   - `ninja -C build32 -j1` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 所有 War3 runtime 测试均为 `useIsolatedDesktop=true + windowed=true + enforce_video_baseline=false`。
   - 本轮开始前发现 MCP `videoRestorePending=true`，已通过 `stop_war3` 恢复并关闭残留 isolated desktop；本轮结束时 `videoRestorePending=false`，无残留 War3 进程。
   - `model_runtime_probe` 小闭环最佳样本：
     - `semanticCoreFrameFresh = true`
     - `semanticCoreSourceUnitCount = 16`
     - `semanticCoreSkinnedResolved = 2`（另一次复测为 0，说明 skinned palette 仍有波动）
     - `semanticCoreSkinnedCandidatePoseReadyCount = 10`
     - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 2`
     - `semanticCoreAttachmentRigidResolved = 20`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount = 16`
     - `semanticCoreSubmittedDrawCount = 24`
     - `objectFallbackDrawCount = 0`
   - 后续 `scene_catchup` / `dynamic_shadow_pressure` smoke：
     - core/native staging 可到 `semanticCoreSubmittedDrawCount = 31/32`
     - `nativeD3D9BackendHasDevice = true`
     - `nativeD3D9BackendSubmittedDrawCount = 31/32`
     - `nativeD3D9BackendExecutedDrawCount = 0`
     - `semanticSceneSubmitted` 仍停在旧值 `4`
4. 本轮新结论
   - 上游 semantic data chain 已能闭环到 fresh：当前已经不是 owner/resource/pose 缺口。
   - root/unit supplement 全量 96 条会导致 build fresh 追不上；小闭环 16 条可以完成并输出 draw packets。
   - `upperLayerEmitted=0` 不再是最直接 blocker；更靠近的 blocker 是 render scene pass 没有消费 control-plane 后建好的 fresh frame。
   - 隔离桌面尾帧里，pipe refresh 能更新 `ShadowRendererCore` 和 native staging counters，但不会自动触发新的 DXVK scene submission tick，因此 `semanticSceneSubmitted` 仍停在旧 frame。
5. 当前 blocker 与下一步
   - 下一轮固定只追 render-thread scene consumption：
     - 确认 `War3TryPopulateSemanticShadowScene` 在真实 render tick 中是否看到 fresh `sourcePublishRevision`。
     - 如果隔离桌面尾帧没有 render tick，需要 AutoTest 增加“等待/触发真实 render-frame advance”的能力，而不是继续用 pipe query 误判 scene submission。
   - 若真实 render tick 已发生但 `semanticSceneSubmitted` 仍为 4，则继续查 `War3ShouldSubmitSemanticPacket` 的 unitsOnly/objectKind filter。
   - skinned palette 仍需稳定化：当前 cap16 下 skinned 可从 0 到 2 波动，下一轮要记录 `skippedNoRuntimeGroupPalette` 的具体 resource/runtime 分布，但不重开 owner 路线。

#### 9.11.16 2026-04-24 Phase 4 scene-consumption gate：确认 blocker 是 render scene pass 未消费 fresh frame

1. 本轮代码
   - `d3d9_war3_scene.h` / `d3d9_device.cpp`
     - 为 semantic scene populate 增加最后一次消费诊断：
       - `semanticScenePopulateAttemptCount`
       - `semanticSceneStatsPublishCount`
       - `semanticSceneLastFrameSerial`
       - `semanticSceneLastSourcePublishRevision`
       - `semanticSceneLastTargetPublishRevision`
       - `semanticSceneLastInputDrawCount`
       - `semanticSceneLastSubmittedDrawCount`
       - `semanticSceneCatchupAttemptCount`
       - `semanticSceneCatchupSuccessCount`
       - `semanticScenePublishRevisionLag`
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp` / `war3_diagnostics_hub.*`
     - 上述 scene-consumption counters 已进入 `get_shadow_runtime_summary` 和 runtime status。
     - `wait_until` 新增 `requireSemanticSceneConsumed` gate：只有 DXVK scene pass 消费到最新 semantic publish revision 才能通过视觉验收。
   - `AutoTest/war3_autotest_mcp.py`
     - hot-shadow gate 现在会把“core 已生成但 scene 未消费”的情况标成 `core-fresh-waiting-render-scene`。
     - `dynamic_shadow_pressure` / `low_pressure_static_reuse` 默认要求 scene consumed；`model_runtime_probe` 仍允许作为 core contract 诊断。
     - shadow 场景 preset 默认改为 `windowed + isolated desktop + enforceVideoBaseline=false`，避免再次触发全屏/视频注册表残留风险。
2. fresh 工件
   - `AutoTest/artifacts/codex_scene_consumption_poll_20260424_1020.json`
   - `AutoTest/artifacts/codex_scene_consumed_gate_20260424_1029.json`
3. 关键输出
   - `ninja -C build32 -j1` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 所有本轮手动 runtime 验证均为 `useIsolatedDesktop=true + windowed=true + enforce_video_baseline=false`。
   - 收尾 `videoRestorePending=false`，无残留 War3 进程。
   - 10 秒采样显示：
     - `frameIndex` 稳定停在 `889`
     - `render.isInGame=false`，但 `inGameRenderReady=true`
     - `semanticSceneStatsPublishCount=1`
     - `semanticScenePopulateAttemptCount=1`
     - scene 最后消费：`semanticSceneLastFrameSerial=887`、`semanticSceneLastSourcePublishRevision=37`
     - core 已推进：`semanticCoreFrameSerial=888`、`semanticCoreSourcePublishRevision=38`
     - `semanticScenePublishRevisionLag=1`
   - `requireSemanticSceneConsumed=true` 的 DLL 级 gate 返回 `wait_until stalled`，并正确携带：
     - `semanticSceneLastSourcePublishRevision=37`
     - `semanticCoreSourcePublishRevision=38`
     - `semanticSceneLastSubmittedDrawCount=4`
     - `semanticScenePublishRevisionLag=1`
4. 本轮新结论
   - 这轮把“packet 被 filter 掉”和“没有真实 render scene pass”分开了：当前证据支持后者。
   - fresh semantic core frame 已经能在 pipe/control-plane 侧继续推进，但隔离桌面尾帧没有新的 render scene submission tick，所以 scene 仍停在旧 revision。
   - 当前不应再回头扩大 owner/resource/pose 逆向范围；视觉 gate 下一步要么触发真实 frame advance，要么让 AutoTest 明确报告“core fresh，scene pending”。
   - 本轮 named-scenario 验证时 MCP 服务器仍使用旧 Python preset，因此结果里还能看到 `enforceVideoBaseline=true`；源码已改为 false，需重启/重载 MCP 后生效。
5. 当前 blocker 与下一步
   - 下一轮主线：修 `wait_for_game_ready` / hot-shadow 流程对 `render.isInGame=false + frame stalled` 的处理。
   - 若隔离桌面无法持续 render tick，需要 AutoTest 在视觉 gate 中直接给出 `core-fresh-waiting-render-scene`，不要误判为 semantic core 失败。
   - 若能触发 frame advance 后 scene 仍不消费 fresh revision，再回到 `War3ShouldSubmitSemanticPacket` 的 `unitsOnly/objectKind` 过滤链。

#### 9.11.17 2026-04-24 Phase 4 AutoTest gate 收口：render-tail pending 已变成明确阶段

1. 本轮代码
   - `AutoTest/war3_autotest_mcp.py`
     - `wait_for_hot_shadow_frame(prefer_summary_poll=true)` 改为主走 `get_hot_shadow_probe`，一次拿到 runtime status、frame manifest 和 shadow summary。
     - 新增 runtime/frame-tail 诊断字段：
       - `runtimeFrameIndex`
       - `runtimeFrameNumber`
       - `runtimeFramePublishRevision`
       - `runtimeRenderInGameReady`
       - `runtimeRenderIsInGame`
       - `runtimeFrameAdvanceStalled`
       - `runtimeFrameStallSec`
       - `runtimeRenderTailStalled`
     - summary-poll 超时/pipe timeout 时保留最后一份有效 `shadowRuntimeSummary`，避免最后一次空响应把真正 counters 清掉。
     - 当 `semanticCoreSubmittedDrawCount > 0` 且 scene revision 落后时，即使 skinned/attachment strict gate 还没通过，也会优先标记：
       - `core-fresh-waiting-render-scene`（core fresh 时）
       - `core-packets-waiting-render-scene`（已有 packet，但 core freshness 仍等下一次 render tick 时）
     - `run_quick_autotest` 将上述两个 phase 统一映射为 `stage=hot-shadow-render-scene-pending`。
   - `war3_control_plane.cpp`
     - `wait_until stalled` 响应补充：
       - `frameStalled`
       - `semanticBuildStalled`
       - `requestedSemanticFrameBuild`
       - `semanticBuildRequestReason`
       - `requireSemanticSceneConsumed`
       - `semanticSceneWaitingForRenderPass`
2. fresh 工件
   - `AutoTest/artifacts/codex_hot_gate_direct_probe_20260424_1044.json`
   - `AutoTest/artifacts/codex_hot_gate_dynamic_visual_probe_20260424_1058.json`
   - `AutoTest/artifacts/codex_run_quick_dynamic_scene_pending_20260424_1102.json`
3. 关键输出
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `ninja -C build32 -j1` 通过。
   - 所有 runtime 验证均使用：
     - `use_isolated_desktop=true`
     - `windowed=true`
     - `enforce_video_baseline=false`
     - `avoid_focus_on_stop=true`
   - 收尾状态：
     - `war3Alive=false`
     - `videoRestorePending=false`
   - `model_runtime_probe` direct gate：
     - `hotOk=true`
     - `semanticSceneConsumptionFresh=true`
     - `semanticCoreSubmittedDrawCount=5`
     - `semanticSceneLastSubmittedDrawCount=4`
     - `runtimeRenderIsInGame=false`
   - `dynamic_shadow_pressure` strict visual gate：
     - `stage=hot-shadow-render-scene-pending`
     - `semanticShadowPhase=core-packets-waiting-render-scene`
     - `runtimeRenderTailStalled=true`
     - `semanticScenePublishRevisionLag=1`
     - `semanticCoreSubmittedDrawCount=1`
     - `semanticSceneLastSubmittedDrawCount=4`
     - `semanticCoreSkinnedResolved=0`
     - `semanticCoreAttachmentRigidResolved=0`
     - `nativeD3D9BackendExecutedDrawCount=0`
4. 本轮新结论
   - AutoTest 现在可以把三件事分开报：
     - semantic core 是否已经产包。
     - DXVK scene pass 是否消费最新 semantic revision。
     - 当前是否卡在 isolated desktop 尾帧无 frame advance。
   - `dynamic_shadow_pressure` 这轮失败不再是“上游模型/身份数据层又丢了”，而是明确的 `hot-shadow-render-scene-pending`。
   - 当前 strict visual gate 还没签收，但失败原因已经足够明确：core 有 packet，scene 仍停旧 revision，runtime tail 没继续推进真实 render frame。
5. 当前 blocker 与下一步
   - 下一轮固定只做“真实 render tick / scene consumption”：
     - 先确认 isolated desktop + windowed 下为什么 `render.isInGame=false` 且 frameIndex 停住。
     - 优先尝试安全的 frame advance 触发或等待策略，不切回全屏，不改视频注册表 baseline。
     - 如果能拿到新的 render tick 但 `semanticScenePublishRevisionLag` 仍不归零，再查 `War3ShouldSubmitSemanticPacket` 的 `unitsOnly/objectKind` filter。
   - 不再重开 owner/resource/pose/palette 逆向；当前 blocker 已经转到 render pass 驱动和视觉 gate。

#### 9.11.18 2026-04-24 Phase 4/native validation：native draw 已执行，DXVK scene gate 仍卡旧 revision

1. 本轮代码
   - `war3_renderer.cpp`
     - `War3Renderer::EndFrame()` 在 capture live state 之后显式 `requestLatestFrameBuild()`，再 `ensureLatestFrameBuilt()`。
     - 对小型 supplemented manifest 增加最多 3 个 render-thread observe chunk，用于让 native validation 消费同一个 render-thread semantic frame，而不是等 control-plane 后续补 build。
   - `war3_shadow_backend_native_d3d9.*` / `war3_shadow_native_runtime.*`
     - 新增 native execute counters：
       - `executeAttemptCount`
       - `executeSuccessCount`
       - `executeSkippedNoDeviceCount`
       - `executeSkippedNoDrawsCount`
       - `lastExecuteSubmittedDrawCount`
       - `lastExecuteFailedDrawCount`
       - `lastSuccessfulExecutedFrameSerial`
       - `lastSuccessfulExecutedDrawCount`
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - 上述 native execute counters 已进入 `get_shadow_runtime_summary`。
   - `war3_autotest_mcp.py`
     - 隔离桌面截图路径已改为先通过 `EnumDesktopWindows` 找主窗口 hwnd，再传给 PowerShell；避免 isolated desktop 下 `Get-Process.MainWindowHandle=0` 导致截图失败。
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_native_execute_counters_20260424_1157.summary.json`
   - 对照前序 raw 工件：
     - `AutoTest/artifacts/codex_model_runtime_probe_tail_counters_20260424_113900.json`
     - `AutoTest/artifacts/codex_dynamic_native_endframe_execute_probe_20260424_112945.json`
3. 关键输出
   - `ninja -C build32 -j1` 通过。
   - 所有 runtime 验证继续使用：
     - `useIsolatedDesktop=true`
     - `windowed=true`
     - `enforceVideoBaseline=false`
     - `avoid_focus_on_stop=true`
   - 11:55 direct MCP probe 显示：
     - `semanticCoreFrameFresh=true`
     - `semanticCoreSubmittedDrawCount=22`
     - `semanticCoreAttachmentRigidResolved=20`
     - `objectFallbackDrawCount=0`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=22`
     - `nativeD3D9BackendExecuteAttemptCount=58`
     - `nativeD3D9BackendExecuteSuccessCount=58`
     - `nativeD3D9BackendExecuteSkippedNoDeviceCount=0`
     - `nativeD3D9BackendExecuteSkippedNoDrawsCount=0`
     - `nativeD3D9BackendLastExecuteFailedDrawCount=0`
   - 同轮仍显示 DXVK scene visual gate 未消费 fresh revision：
     - `semanticScenePopulateAttemptCount=1`
     - `semanticSceneLastSourcePublishRevision=37`
     - `semanticCoreSourcePublishRevision=4294967338`
     - `semanticScenePublishRevisionLag=4294967301`
   - 12:00 复跑出现一次 ready 阶段 pipe timeout：`named pipe 不可用: 2`，但无新 crash dump、无残留 War3、`videoRestorePending=false`。
4. 本轮新结论
   - native D3D9 backend 已经不只是“能 build packet / upload geometry”，而是实际发生了 native Draw 调用并成功返回。
   - 旧的 `nativeD3D9BackendExecutedDrawCount=0` 是统计口径问题：后续 control-plane `buildLatestFrame()` 会准备新 frame 并重置 current executed count，导致“最后一次成功执行”被覆盖。
   - 当前项目不再卡在上游 owner/resource/pose，也不再卡在 native backend 能否执行 draw；剩余 blocker 是 DXVK validation visual scene pass 仍只消费旧 revision，isolated desktop 尾帧没有新的 scene populate。
5. 当前 blocker 与下一步
   - 下一轮不要再重开 owner 逆向，也不要继续扩大 semantic data probes。
   - 优先处理两个收口点：
     - AutoTest：ready pipe timeout 需要和 crash/未初始化/启动慢区分，防止偶发 pipe 不可用被误读成 semantic 失败。
     - Visual gate：DXVK scene gate 仍停旧 revision；若继续保持 isolated desktop，应该把 native successful execute 作为 native path 验收信号，同时把 DXVK scene pending 单独报成 validation-host tail-frame 限制。
   - 若要继续推进最终目标，下一轮可从 native D3D9 path 往前走：把 `lastSuccessfulExecutedDrawCount > 0` 纳入 native smoke gate，再处理 late-inject/native host 的实际可见性与渲染状态隔离。

#### 9.11.19 2026-04-24 AutoTest gate 更新：native executed 与 DXVK scene pending 分离

1. 本轮代码
   - `AutoTest/war3_autotest_mcp.py`
     - 新增 `_native_execute_success_draw_count()`。
     - native execute gate 不再只看会被后续 frame prepare 重置的 `nativeD3D9BackendExecutedDrawCount`。
     - 优先使用 `nativeD3D9BackendLastSuccessfulExecutedDrawCount`；若旧 DLL 尚未带该字段，则回退到 `ExecuteSuccessCount + LastExecuteSubmitted/Failed`。
     - 当 native backend 已成功执行 semantic draws，但 DXVK validation scene 仍未消费 fresh revision 时，报告：
       - `semanticShadowPhase=native-semantic-executed-scene-pending`
       - `semanticNativeExecuted=true`
       - `nativeD3D9BackendEffectiveExecutedDrawCount`
2. 验证
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 本轮只改 AutoTest 分类口径，未再次启动 War3；runtime 证据继续参考 11:55 MCP direct probe 与 summary artifact。
3. 新结论
   - 后续无人值守报告可以明确区分：
     - native semantic path 已经实际执行 draw。
     - DXVK validation scene pass 仍处于 stale revision / tail-frame pending。
   - 这能避免把 native D3D9 的阶段性成功误报为“动态阴影数据链失败”。
4. 下一步
   - 下一轮 runtime 验证如果 ready pipe 稳定，应观察 `nativeD3D9BackendEffectiveExecutedDrawCount > 0` 是否进入报告。
   - 若继续推进 native path，优先处理 native actual visibility / render-state isolation，而不是继续扩 owner/resource/pose 逆向。
#### 9.11.20 2026-04-24 dynamic shadow consume/path-count 收口

1. 本轮代码
   - `ShadowValidationRuntime::snapshotRenderableFrameShared()` 改为短窗口内优先保留含 skinned draw 的完整动态帧，不再只按 publish revision 让较新的 partial rigid frame 抢占消费。
   - `War3TryPopulateSemanticShadowScene()` 增加 scene frame 评分：按 skinned draw 数、可提交 draw 数、总 draw 数、revision 选择 DXVK scene 消费帧，避免低 revision supplemented rigid frame 盖掉 canonical frame。
   - `War3Renderer::EndFrame()` 增加 native catch-up execute：Stage11 仍是主执行点；若 EndFrame 才准备出一个尚未执行过的 semantic frame，则只补执行一次，避免回到重复 execute storm。
   - `ShadowRendererCore::resolveRecord()` 调整为 skinned 优先，skinned pose/palette 失败时再回落 attachment rigid；避免 attachment rigid 永久抢占 canonical skinned 路线。
   - supplemental dedup 从 `unordered_set` 改成 key -> draw index；若同 key 已有 rigid，而后续解析出 skinned packet，则用 skinned 替换 rigid。
   - control-plane 新增 frame path 观测字段：`semanticCoreLastFrame*` 与 `semanticCoreRenderableFrame*`，直接暴露最终 frame 里实际 draw/skinned draw 数。
2. fresh 工件
   - `AutoTest/artifacts/codex_dynamic_frame_selection_probe_20260424_173817.json`
   - `AutoTest/artifacts/codex_dynamic_scene_frame_score_probe_20260424_174226.json`
   - `AutoTest/artifacts/codex_dynamic_endframe_catchup_probe_20260424_174747.json`
   - `AutoTest/artifacts/codex_dynamic_frame_path_counts_probe_20260424_175515.json`
   - `AutoTest/artifacts/codex_dynamic_skinned_first_probe_20260424_175859.json`
   - `AutoTest/artifacts/codex_dynamic_skinned_then_rigid_fallback_probe_20260424_180314.json`
   - `AutoTest/artifacts/codex_dynamic_partial_no_overwrite_probe_20260424_180650.json`
   - `AutoTest/artifacts/codex_model_runtime_contract_after_skinned_dedup_20260424_181343.json`
   - `AutoTest/artifacts/codex_dynamic_semantic_rigid_floor_probe_20260424_181613.json`
3. 验证
   - `ninja -C build32 -j1` 通过。
   - 所有 War3 runtime 验证继续使用 isolated desktop + windowed + `enforce_video_baseline=false`，未触发全屏路径。
   - 当前 semantic rigid floor 仍成立：
     - `semanticCoreFrameFresh=true`
     - `semanticCoreResolved=5`
     - `semanticCoreLastFrameDrawCount=5`
     - `semanticSceneLastSubmittedDrawCount=4`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount=4`
     - `objectFallbackDrawCount=0`
4. 新硬结论
   - native 执行时机的一帧落后问题已部分收口：EndFrame catch-up 后 `LastSuccessfulExecutedFrameSerial` 可以追到当前 semantic frame。
   - `semanticCoreSkinnedResolved` 曾能抬到 17，但最终 `ShadowSubmissionFrame.draws` 仍可能只有 rigid；根因不是“完全没有 skinned 理论”，而是 skinned resolve、supplemental dedup、partial frame publish 三者之间存在断层。
   - partial build 曾把 canonical `m_lastFrame` 覆盖成 1 个 rigid draw，导致统计显示 skinned resolved、消费层却只拿到 rigid；本轮已禁止 partial 覆盖 canonical frame。
   - 当前稳定可用底线仍是 semantic-only rigid/native execution，旧 VB/IB snapshot/freeze fallback 没有回到主路径。
5. 当前 blocker
   - 完整动态 skinned 阴影尚未签收；最新 dynamic smoke 仍为 `semantic-rigid-ok-skinned-pending`。
   - 需要继续追 skinned candidate 的 pose key：短窗口里仍出现 `semanticCoreSkinnedCandidateCount>0` 但 `semanticCoreSkinnedCandidatePoseReadyCount=0` 的样本。
   - 下一轮不要重开 owner 逆向；优先在 `TryResolveBestPoseForRenderable()` 的 no-pose detail 上，把 scene/world/resource candidate 对到稳定 runtime pose，或者明确标成只能 rigid 的对象。
6. 下一步精确入口
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
     - `TryResolveBestPoseForRenderable()`
     - `TryResolvePoseByResourceRuntimeTree()`
     - `TryBuildRuntimeGroupPalette()`
   - 目标是让 `semanticCoreLastFrameSkinnedDrawCount > 0`、`semanticSceneSubmittedSkinned > 0`、`nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount > 0` 同时成立。

#### 9.11.21 2026-04-24 semantic skinned dynamic shadow 首轮闭环

1. 本轮代码
   - `ShadowValidationRuntime::ShouldPreferRenderableSubmissionFrame()` 改为短窗口内优先保留含 skinned draw 的完整动态帧，避免较新的 rigid-only partial frame 覆盖可绘制 skinned frame。
   - `ShadowFrameManifest` root/unit supplement 现在会消费 `PoseRegistry::snapshot()` pose seed；当 runtime 有 pose/world-transform 信号但没有逻辑 unit identity 时，允许作为 anonymous semantic renderable 进入 contract，不伪造 `jHandle/rawcode/ObjectKind`。
   - `TryResolvePoseByRuntimeModelSnapshotOrLive()` 不再遇到 base runtime pose 就立即返回，而是在 base/live/`+0xA0`/`-0xA0` alias 之间选择更完整的 pose palette。
   - `TryBuildRuntimeGroupPalette()` 新增 bounded resource-matched pose rescue 与 uniform-palette fallback：当 runtime 只发布 1 个最终矩阵但 geoset 带 vertex-group metadata 时，用该最终矩阵广播成 semantic rigid-palette，以生成 skinned contract packet，而不是回落 VB/IB snapshot。
   - control-plane 新增 runtime group palette miss 细分 counters，能区分 no skinning data / no pose palette / invalid group table / index out of range / fallback failed。
   - AutoTest hot-shadow gate 修正两点：
     - dynamic 场景不再被旧 attachment-rigid 诊断阻塞，只要 strict skinned semantic 成立即可通过。
     - low-pressure 场景在 isolated desktop 尾帧不再推进时，如果 semantic frame 已 fresh、scene 已消费、native 已执行、fallback=0，则记录 `semanticTailFrameAccepted=true` 并通过，不再误报 `wait_until stalled`。
2. fresh 工件
   - `AutoTest/artifacts/codex_dynamic_pose_seed_supplement_probe_20260424_183118.json`
   - `AutoTest/artifacts/codex_dynamic_palette_miss_reason_probe_20260424_184021.json`
   - `AutoTest/artifacts/codex_dynamic_uniform_palette_probe_20260424_185320.json`
   - `AutoTest/artifacts/codex_dynamic_skinned_gate_pass_probe_20260424_185751.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_after_skinned_gate_20260424_190307.json`
   - `AutoTest/artifacts/codex_low_pressure_static_reuse_tail_accept_20260424_190931.json`
   - `AutoTest/artifacts/codex_dynamic_shadow_pressure_final_semantic_skinned_20260424_191212.json`
3. 三场景验证
   - `ninja -C build32 -j1` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 所有 War3 runtime 验证继续使用：
     - `useIsolatedDesktop=true`
     - `windowed=true`
     - `enforce_video_baseline=false`
     - `avoid_focus_on_stop=true`
   - `model_runtime_probe`：
     - `ok=true`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreResolved=70`
     - `semanticCoreSkinnedResolved=56`
     - `semanticSceneSubmittedSkinned=37`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=27`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount=32`
     - `objectFallbackDrawCount=0`
   - `low_pressure_static_reuse`：
     - `ok=true`
     - `semanticTailFrameAccepted=true`
     - `semanticCoreResolved=32`
     - `semanticCoreSkinnedResolved=28`
     - `semanticCoreLastFrameSkinnedDrawCount=28`
     - `semanticSceneSubmittedSkinned=45`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=28`
     - `objectFallbackDrawCount=0`
   - `dynamic_shadow_pressure`：
     - `ok=true`
     - `semanticCoreResolved=68`
     - `semanticCoreSkinnedResolved=54`
     - `semanticCoreRenderableFrameSkinnedDrawCount=53`
     - `semanticSceneSubmittedSkinned=53`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=27`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount=32`
     - `objectFallbackDrawCount=0`
4. 新硬结论
   - 当前已经不再只是 semantic-only rigid floor；semantic skinned packet 已进入 core frame、DXVK validation scene summary、native D3D9 backend execute counters。
   - 旧 VB/IB snapshot/freeze fallback 没有回到 object shadow 主路径；本轮三场景 `objectFallbackDrawCount=0`。
   - `resource/geoset + pose/world-transform` 数据链已经足够生成动态 semantic skinned submission；owner identity 不是本轮 blocker。
   - uniform-palette fallback 是当前“可用动态阴影”桥：它让只发布最终 root/world matrix 的 runtime 也能按 semantic contract 出 skinned packet；后续需要用视觉/姿态质量继续确认它在复杂动画上的误差。
5. 当前 blocker
   - 性能报告的 `frameCount/avgFps` 在 isolated desktop + windowed 本轮仍为 0；报告里能看到 shadow budget 和 semantic counters，但不能直接作为 FPS 结论。
   - 下一轮需要先修 AutoTest/perf 采样口径：在不回到全屏、不抢前台的前提下拿到真实 frame timing/FPS。
   - 若 FPS 样本恢复后 CPU 偏高，优先优化 palette/build dedupe 与 semantic frame build chunk，而不是恢复 snapshot/freeze。
6. 下一步精确入口
   - `AutoTest/war3_autotest_mcp.py`
     - 修 isolated desktop + windowed 下 perf report `frameCount=0` 的原因。
     - 将 `shadowBudgetSummary.framesObserved` 与 control-plane frame advance 分开展示，避免把 semantic 成功误读成 FPS 成功。
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
     - 继续压缩 uniform-palette fallback 的重复 build。
     - 将 `runtimeModelPtr + frameSerial + matrixHash` palette 结果缓存/去重做到更稳定。
   - `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
     - 继续观察 `semanticCoreBuildDurationUs` / chunk count，作为下一轮性能优化入口。

#### 9.11.22 2026-04-24 手动进图白屏/未响应止血

1. 用户反馈
   - 普通手动进图从原本几秒变成约一分钟。
   - 进图后主画面全白，并且 Warcraft III 未响应。
   - 用户日志显示普通路径持续输出：
     - `DXVK SemanticCore: no-pose detail ...`
     - `DXVK SemanticCore: build-progress ...`
     - `DXVK SemanticCore: build ...`
     - `DXVK War3Shadow: semantic scene submitted ...`
     - `DXVK SemanticDispatchGate: phase=fallback mode=0 reason=1 ...`
2. 根因判断
   - 上一轮为 DXVK validation host 签收 semantic skinned path，将实验 consumer 默认打开到了普通游戏路径：
     - `kShadowSemanticCoreValidationEnabled=true`
     - `kShadowSemanticCoreSceneSubmissionEnabled=true`
     - `kShadowSemanticCoreSceneBootstrapCatchupEnabled=true`
     - `kShadowSemanticCoreSceneEndFrameFlushEnabled=true`
     - `kShadowSemanticCoreSceneBypassLegacyUnitCaptureEnabled=true`
     - `kNativeRendererHostExecuteValidationEnabled=true`
     - `kNativeSemanticShadowWorldStageValidationEnabled=true`
   - 这会让正常 CWorld/Stage11/BeforeUi/EndFrame 路径也持续构建 semantic frame、提交 DXVK scene、执行 native validation draw。
   - 用户日志中的 `pose=0/runtime=0` transparent/UI-like object 反复参与 semantic build，说明这条实验构建链已经污染普通手动游玩热路径。
3. 止血代码
   - `src/d3d9/war3/core/war3_internal_test_config.h`
     - 将 semantic core validation 默认改为 `false`。
     - 将 semantic scene submission 默认改为 `false`。
     - 将 bootstrap catchup 默认改为 `false`。
     - 将 EndFrame semantic scene flush 默认改为 `false`。
     - 将 legacy unit capture bypass 默认改为 `false`。
     - 将 native renderer host execute validation 默认改为 `false`。
     - 将 native semantic world-stage validation 默认改为 `false`。
4. 保留内容
   - 本轮没有删除上游数据采集 hook：
     - visible/renderable manifest 采集仍保留。
     - model resource / runtime model / pose / attachment registry 仍保留。
     - control-plane / summary 仍保留。
   - 关闭的是“消费/提交/执行实验路径”的默认启用，避免普通进图被 semantic build 拖死。
5. 验证
   - `ninja -C build32 -j1` 通过。
   - 普通 smoke：
     - fresh 工件：`AutoTest/artifacts/codex_semantic_default_off_smoke_20260424_192626.json`
     - `ready.ok=true`
     - `ready.mode=control-plane`
     - `ready.elapsedSec=17.492`
     - `stop.ok=true`
   - 最新 `war3_d3d9.log` 未再检出 `SemanticCore / SemanticShadow / NativeSemantic / host validation executed` 刷屏。
6. 下一步
   - 后续 semantic skinned / native validation 不能再靠编译期默认开关污染普通游戏。
   - 下一轮应新增显式运行时开关或 AutoTest 专用 profile，例如 `DXVK_WAR3_ENABLE_SEMANTIC_SHADOW_EXPERIMENT=1`，让实验路径只在 isolated/windowed 自动化里启用。
   - 在完成显式开关前，普通手动游玩以稳定为第一优先。

#### 9.11.23 2026-04-25 DXVK visual preview 收口与踩坑记录

1. 完成内容
   - `DXVK_WAR3_SEMANTIC_SHADOW_BYPASS_LEGACY_UNIT_CAPTURE=1` 现在可以在显式 semantic preview 下打开 legacy unit fallback 旁路；普通游戏默认仍关闭。
   - `DXVK_WAR3_SEMANTIC_SHADOW_PRE_READY=1` 现在单独控制 pre-ready semantic validation；普通 preview 默认必须等 `gameStarted`，避免进图前抢热路径。
   - AutoTest hot-shadow gate 新增 `semanticTailSceneAccepted` / `semanticSceneOnlyAccepted`，能把“DXVK semantic scene 已提交并消费”与 native D3D9 backend execute 分开验收。
2. fresh 工件
   - `AutoTest/artifacts/codex_low_pressure_visual_endframe_semantic_20260425.json`
   - `AutoTest/artifacts/screenshots/war3_20260425_001727.png`
   - `AutoTest/artifacts/codex_low_pressure_visual_beforeui_sceneonly_20260425.json`
   - `AutoTest/artifacts/screenshots/war3_20260425_002825.png`
   - `AutoTest/artifacts/codex_low_pressure_semantic_off_ready_baseline_20260425.json`
   - `AutoTest/artifacts/screenshots/war3_20260425_003856.png`
   - `AutoTest/artifacts/codex_low_pressure_visual_beforeui_cap32_20260425.json`
   - `AutoTest/artifacts/screenshots/war3_20260425_003503.png`
3. 硬结论
   - EndFrame out-of-band flush 可提交 `semanticSceneLastSubmittedDrawCount=19`、`semanticSceneSubmittedSkinned=19`、`objectFallbackDrawCount=0`，但视觉上会出现左侧大块黑影/画面污染，不可作为最终路径。
   - 真实 BeforeUi scene-only 路线画面干净，已拿到截图；当前稳定小闭环为 `semanticSceneLastSubmittedDrawCount=9`、`semanticSceneSubmittedSkinned=9`、`objectFallbackDrawCount=0`。
   - 当前低压图在 semantic-off 普通路径下也约 60 秒才触发 control-plane ready，因此本轮看到的低 FPS overlay / ready 慢不能全部归因于 semantic consumer。
   - 把 scene build 首帧 catchup 从 8 pass 提到 32 pass 会直接打坏 ready；把 build chunk budget 从 4ms 提到 8ms 没提高提交数，反而拖慢 ready。两条路均已证伪并退回。
4. 当前 blocker
   - 视觉上已经不是“没有阴影”，而是“BeforeUi 真实路径只稳定提交 9 个 skinned semantic draw，还没有完整覆盖全部蒙皮单位”。
   - 不能用 EndFrame flush 作弊替代 BeforeUi；它会污染主画面。
   - 下一轮应优化 manifest record 选择/已完成帧复用，让 BeforeUi 在不增加同步 pass/budget 的情况下优先消费更高价值的 skinned records。
5. 下一步精确入口
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
     - 优化 `MaybeCapPreviewManifest()`，优先选择 pose/resource/geoset 都齐的 unit/skinned records，而不是按原 manifest 顺序吃低价值记录。
   - `src/d3d9/d3d9_device.cpp::War3TryPopulateSemanticShadowScene`
     - 保持 `kSceneSupplementedBuildMaxPasses=8`；不要再提高到 32。
   - `AutoTest/war3_autotest_mcp.py`
     - 继续保留 scene-only gate；native D3D9 backend execute 不再阻塞 DXVK visual validation。

#### 9.11.24 2026-04-25 semantic preview cap 优先级与低压图基线分离

1. 完成内容
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
     - `MaybeCapPreviewManifest()` 不再只按 `Unit -> Building -> Destructible` 粗排。
     - capped preview 现在优先选择 `unit + resolved geoset + ready resource + pose palette + skinned data` 的记录。
     - pose 可用性改为一次性索引 runtime/scene/unit 指针集合，不再在排序打分中复制 `matrixPalette`。
   - 该改动不提高 build pass，不提高 chunk budget，不恢复 snapshot/freeze/VB/IB fallback。
2. 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - fresh 工件：
     - `AutoTest/artifacts/codex_low_pressure_beforeui_prioritized_cap32_20260425.json`
     - `AutoTest/artifacts/screenshots/war3_20260425_010530.png`
     - `AutoTest/artifacts/codex_low_pressure_semantic_off_compare_20260425.json`
     - `AutoTest/artifacts/screenshots/war3_20260425_011023.png`
     - `AutoTest/artifacts/codex_low_pressure_dxvk_only_no_benchmark_20260425.json`
     - `AutoTest/artifacts/screenshots/war3_20260425_011641.png`
     - `AutoTest/artifacts/codex_shadowtest_old_map_dxvk_only_no_benchmark_20260425.json`
     - `AutoTest/artifacts/screenshots/war3_20260425_012641.png`
3. 硬结论
   - semantic preview cap 排序后，低压图 report 中 `semanticSceneSubmittedSkinned` 已可到 `32`，`objectFallbackDrawCount=0`。
   - 同一张 `光影测试低压.w3x` 在 `semantic preview=0`、`runtime benchmark=0`、`profile=dxvk_only` 下仍约 `56s` 才 ready，画面 FPS overlay 仍为 `0.6`。
   - 同目录旧图 `E:/Work/War3/Maps/ShadowTest/光影测试.w3x` 在相同 `dxvk_only + benchmark off` 条件下约 `10.878s` ready，说明当前 2026-04-24 20:27 修改后的低压图本身/地图脚本是低 FPS 与慢 ready 的重要变量，不能再把该现象单独归因到 semantic consumer。
   - 本轮超时的 `光影测试.w3x` 对照已清理残留进程；收尾无 War3/IDA 残留。
4. 当前 blocker
   - semantic 数据消费已经能提交更多 skinned packet，但视觉截图仍很难肉眼区分“新 semantic 阴影”和旧/原生阴影。
   - 性能验收不能继续使用当前修改后的低压图作为唯一依据；它在 semantic off / dxvk_only 下也慢。
   - 下一轮需要分两条线：
     - 用旧低压图或一个确定无 JASS 重负载的微场景做视觉/perf gate。
     - 单独检查当前 `光影测试低压.w3x` 的 JASS 初始化/锁定目标逻辑，确认是否有长初始化或低频 FPS overlay 误导。
5. 下一步精确入口
   - `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
     - 继续做 completed-frame reuse / scene frame selection，不再提高 pass/budget。
   - `AutoTest/war3_autotest_mcp.py`
     - 给 visual test 增加“semantic correctness passed but map baseline slow”的分类，避免把地图脚本慢误判成渲染慢。
   - `E:/Work/War3/Maps/ShadowTest/光影测试低压.w3x`
     - 若要继续作为主视觉验收图，需要先验证其 JASS 初始化是否会把 ready 拖到约 56 秒。

#### 9.11.25 2026-04-25 runtime profile 分块关闭开关

1. 完成内容
   - 新增 runtime module：`semantic.data`。
   - `DXVK_WAR3_DISABLE=semantic.data` 现在会关闭上层 semantic/model 数据链的主要热路径：
     - `war3_model_hook` 不再安装 runtime model / pose hook。
     - `War3Renderer` 不再 begin/end semantic registries。
     - `ShadowRuntimeContractCache::captureLiveState()` 不再在普通帧/semantic scene/native validation 路径被触发。
     - semantic scene bypass candidate / native semantic backend 入口会被拒绝。
   - 渲染层 runtime modules 现在实际控制更多入口：
     - `shadow.capture` 会阻止 `War3TryCaptureShadowCaster()` 进入旧捕获热路径。
     - `shadow.receiver` / `ssao` / `aa` 会控制对应 pipeline pass 注册和执行。
     - `postfx` 会控制 ShaderPack/PostProcess 执行。
   - `DXVK_WAR3_DISABLE=render` 新增为排查别名：
     - 等价于 `hook.render,render.queue,shadow.capture,shadow.map,shadow.receiver,shadow.taa,postfx,ssao,aa`。
   - AutoTest 模块矩阵已同步 `semantic.data`，并新增 `sub_no_semantic_data` case。
2. 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
3. 推荐排查顺序
   - `DXVK_WAR3_PROFILE=dxvk_only`
     - 纯 DXVK 基线，先确认地图/JASS 自己是否已经慢。
   - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=render,semantic.data`
     - 保留 control-plane/非渲染 hook，关闭渲染干涉和上层语义数据。
   - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=render`
     - 只关闭渲染层，单独测上层语义数据链开销。
   - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=shadow`
     - 关闭 shadow capture/map/receiver，但保留 postfx/AA/SSAO。
   - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=semantic.data`
     - 保留渲染层，单独关闭上层 semantic 数据链。
   - `DXVK_WAR3_PROFILE=full_default`
     - 全量默认路径，用于和前面结果对比。
4. 当前 blocker
   - 现在可以按模块稳定切分性能来源；下一步应由用户或 AutoTest 用同一张地图按上述顺序跑 ready 时间、响应状态、FPS/截图。
   - 若 `dxvk_only` 仍慢，优先查地图/JASS/锁定镜头逻辑；若 `render,semantic.data` 快而 `render` 慢，问题在上层 semantic 数据链；若 `render` 快但 `shadow` 慢，问题在 shadow receiver/capture/postfx 侧。

#### 9.11.26 2026-04-25 internal_test_config 编译期开关对齐

1. 用户澄清
   - 本轮用户要的是修改 `src/d3d9/war3/core/war3_internal_test_config.h`，不是只靠环境变量或 AutoTest profile。
2. 完成内容
   - `war3_internal_test_config.h` 新增编译期分块排查开关：
     - `kWar3RuntimeConfigDxvkOnlyBaseline`
     - `kWar3RuntimeConfigDisableRenderInterference`
     - `kWar3RuntimeConfigDisableShadowStack`
     - `kWar3RuntimeConfigDisablePostFxStack`
     - `kWar3RuntimeConfigDisableSemanticData`
   - 当前默认已按用户要求设为：
     - `kWar3RuntimeConfigDisableRenderInterference = true`
     - `kWar3RuntimeConfigDisableSemanticData = false`
   - 这意味着当前 build 是“关闭渲染层干涉，保留上层 semantic 数据链”的测试版本。
   - `war3_runtime_profile.cpp` 现在把上述 config 编译进 `runtimeProfile.disabledModules`，优先级高于环境变量。
3. 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
4. 下一步
   - 用户可先直接用当前 DLL 测试。
   - 若仍卡：把 `kWar3RuntimeConfigDisableSemanticData` 改为 `true`，重新 `ninja -C build32 -j4` 后再测。
   - 若不卡：逐步把 `kWar3RuntimeConfigDisableShadowStack`、`kWar3RuntimeConfigDisablePostFxStack`、`kWar3RuntimeConfigDisableRenderInterference` 组合打开/关闭定位具体渲染子模块。

#### 9.11.27 2026-04-25 semantic data 子模块二分排查入口

1. 用户最新实测
   - `kWar3RuntimeConfigDisableSemanticData=true`，旧渲染层保持开启：旧实现渲染层可到约 `100 FPS`。
   - 渲染层也关闭后，原版/DXVK 基线可到约 `250 FPS`。
   - 因此本轮硬结论更新为：白屏、未响应、低 FPS 的主问题不在旧渲染层本体，而在上层 semantic data 链。
2. 完成内容
   - 在 `src/d3d9/war3/core/war3_internal_test_config.h` 继续细分 semantic data 编译期开关：
     - `kWar3RuntimeConfigDisableSemanticModelHooks`
     - `kWar3RuntimeConfigDisableSemanticPoseHooks`
     - `kWar3RuntimeConfigDisableSemanticAttachmentHooks`
     - `kWar3RuntimeConfigDisableSemanticFrameRegistries`
     - `kWar3RuntimeConfigDisableSemanticContractCapture`
     - `kWar3RuntimeConfigDisableSemanticConsumer`
   - `war3_model_hook.cpp` 已按上述开关分段安装 runtime model / pose / attachment hooks。
   - `war3_renderer.cpp`、`d3d9_device.cpp`、`war3_runtime_bootstrap.cpp` 已按 registry / contract capture / consumer 三段分别门控，避免只靠 `semantic.data` 总开关。
3. 当前 build 测试含义
   - 当前配置为：
     - `kWar3RuntimeConfigDisableRenderInterference=false`
     - `kWar3RuntimeConfigDisableSemanticData=false`
     - `kWar3RuntimeConfigDisableSemanticModelHooks=true`
     - `kWar3RuntimeConfigDisableSemanticPoseHooks=false`
     - `kWar3RuntimeConfigDisableSemanticAttachmentHooks=false`
     - `kWar3RuntimeConfigDisableSemanticFrameRegistries=false`
     - `kWar3RuntimeConfigDisableSemanticContractCapture=false`
     - `kWar3RuntimeConfigDisableSemanticConsumer=false`
   - 这是一轮“旧渲染层开启、上层语义总开关开启、只关闭 runtime model hook 包”的二分测试版本。
4. 验证
   - `ninja -C build32 -j4` 通过，本轮为 `ninja: no work to do`。
   - 上一轮已通过 `python -m py_compile AutoTest/war3_autotest_mcp.py`。
5. 下一轮精确测试顺序
   - 先直接测试当前 DLL：
     - 如果 FPS 恢复到接近旧渲染层的约 `100 FPS`，说明性能杀手在 `war3_model_hook` runtime model / pose / attachment hook 包内部。
     - 如果仍然未响应，说明性能杀手不只在 hook 安装包，而要继续测 frame registries / contract capture / consumer。
   - 若当前 DLL 不卡，再按以下顺序打开 hook 包内部：
     - `DisableSemanticModelHooks=false`，`DisableSemanticPoseHooks=true`，`DisableSemanticAttachmentHooks=true`：测 model/resource 基础 hook。
     - `DisableSemanticPoseHooks=false`，`DisableSemanticAttachmentHooks=true`：测 pose/local-output 相关开销。
     - `DisableSemanticAttachmentHooks=false`：测 attachment/child runtime 相关开销。
   - 若当前 DLL 仍卡，再按以下顺序关闭消费侧：
     - `DisableSemanticFrameRegistries=true`
     - `DisableSemanticContractCapture=true`
     - `DisableSemanticConsumer=true`
6. 当前 blocker
   - 已确认不是“旧渲染层导致所有卡顿”；当前 blocker 是找出上层 semantic data 中最重的子链。
   - 下一轮不要再改 shadow receiver/postfx/AA 方向，先按上述 config 二分拿到第一组用户实测结果。

#### 9.11.28 2026-04-25 semantic 子模块依赖收口 + perf summary 面

1. 完成内容
   - 新增 semantic 子模块有效态依赖：
     - `SemanticModelProducerEffective`
     - `SemanticPoseProducerEffective`
     - `SemanticAttachmentProducerEffective`
     - `SemanticFrameRegistriesEffective`
     - `SemanticContractCaptureEffective`
     - `SemanticConsumerEffective`
   - 现在 `DisableSemanticModelHooks=true` 时，下游 pose/attachment/frame registry/contract/consumer 会自动变成无效态，不再允许 consumer 在缺 producer 的状态下继续 build。
   - `War3Renderer`、`d3d9_device.cpp`、`ShadowRuntimeContractCache`、`NativeD3D9BackendRuntime` 查询路径都改为读取有效态，避免“编译期开关半关，但运行时仍进入 semantic build”的状态。
2. control-plane 新增观测面
   - `get_shadow_runtime_summary` 新增：
     - `semanticDataModuleEnabled`
     - `semanticModelProducerEnabled`
     - `semanticPoseProducerEnabled`
     - `semanticAttachmentProducerEnabled`
     - `semanticFrameRegistriesEnabled`
     - `semanticContractCaptureEnabled`
     - `semanticConsumerEnabled`
     - `semanticBuildSkippedReason`
   - 新增 semantic data perf block：
     - `semanticModelHookCalls/us`
     - `semanticPoseHookCalls/us`
     - `semanticAttachmentHookCalls/us`
     - `semanticFrameRegistryPublishCalls/us`
     - `semanticContractCaptureCalls/us`
     - `semanticConsumerBuildCalls/us`
     - `semanticLastHotFunctionTag/us`
   - 当前 model/pose/attachment 先用已有 hook counter 汇总 calls；registry/contract/consumer 已有轻量 microsecond 计时。
3. Fresh 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `git diff --check` 仅有既有 CRLF 提示。
   - isolated desktop + windowed smoke：
     - 工件：`AutoTest/artifacts/codex_semantic_dependency_guard_smoke_20260425.json`
     - `readyElapsedSec=1.447`
     - `frameIndex=4020`
     - `render.isInGame=true`
     - `semanticDataModuleEnabled=1`
     - `semanticModelProducerEnabled=0`
     - `semanticFrameRegistriesEnabled=0`
     - `semanticContractCaptureEnabled=0`
     - `semanticConsumerEnabled=0`
     - `semanticBuildSkippedReason=2`
     - `semanticConsumerBuildCalls=0`
4. 结论
   - 用户之前看到的 “DisableSemanticModelHooks=true 后两帧后未响应” 属于 producer/consumer 半开启缺陷。
   - 当前版本在 model producer 关闭时已经能 fail-fast：游戏没有卡死，帧持续推进，但 hot-shadow gate 会失败，这是预期结果，因为本轮就是关闭 semantic producer/consumer 来验证安全性。
   - 这轮还没有恢复动态阴影功能；它解决的是“二分时不能因为缺数据空转把游戏卡死”的前置问题。
5. 下一步精确测试顺序
   - 先让用户或 AutoTest 测当前 DLL 的实际 FPS：
     - 如果恢复到接近旧渲染层约 `100 FPS`，说明旧的卡死主要来自 producer-off/consumer-on build storm。
   - 下一刀建议：
     - `DisableSemanticModelHooks=false`
     - `DisableSemanticPoseHooks=true`
     - `DisableSemanticAttachmentHooks=true`
     - `DisableSemanticFrameRegistries=true`
     - `DisableSemanticContractCapture=true`
     - `DisableSemanticConsumer=true`
   - 这会只打开 model/resource 基础 hook，不打开 pose/attachment/registry/consumer，用来判断最基础 runtime model hook 本身是否有不可接受开销。

#### 9.11.29 2026-04-25 semantic consumer 热点收口：owner 全表扫描与 attachment 空转

1. 完成内容
   - 为 `ShadowRendererCore` 增加 pose resolve 子阶段耗时：
     - direct / owner / sprite / instance registry / shadow registry / render registry / runtime roots / mesh pose / miss diagnostic。
   - `get_shadow_runtime_summary` 已暴露上述 `semanticCoreSlowestPose*Us` 字段，后续 AutoTest 不需要再靠 DebugView 猜慢点。
   - `ShadowModelResourceCache` 新增 `runtimeGeoset/runtimeGeosetData -> runtimeModel` O(1) owner 索引。
   - `TryResolveBestPoseForRenderable` 增加完整 pose 早退，避免拿到 matrix+world 后继续扫泛化兜底。
   - `TryBuildAttachmentRigidPacket` 增加 cheap precheck，只有存在直接 attachment 候选时才进入完整 attachment rigid 解析。
   - `ShadowRuntimeContractCache::captureLiveState` 增加同帧轻量短路：同一 frameSerial、可见计数不增长、resource revision 不变且已发布 contract 可用时，不再重复做完整 snapshot。
2. Fresh 验证
   - `ninja -C build32 -j4` 通过。
   - `model_runtime_probe`：
     - 工件：`AutoTest/artifacts/codex_semantic_consumer_preview_runtime_owner_index_smoke_20260425.json`
     - owner scan 修复前：`semanticCoreBuildDurationUs≈676507`、`semanticCoreSlowestPoseOwnerLookupUs≈67229`、`semanticCoreSlowestPoseRuntimeRootsUs≈70407`。
     - owner scan 修复后：`semanticCoreBuildDurationUs=6975`、`semanticCoreSlowestPoseOwnerLookupUs=3`、`semanticCoreSlowestPoseRuntimeRootsUs=3441`、`semanticCoreSkinnedResolved=30`、`objectFallbackDrawCount=0`。
   - `光影测试低压.w3x`：
     - 工件：`AutoTest/artifacts/codex_low_pressure_attachment_precheck_20260425.json`
     - attachment precheck 后：`semanticCoreBuildDurationUs=53266`、`semanticConsumerBuildUs=311318`、`semanticCoreSkinnedResolved=132`、`semanticCoreSubmittedDrawCount=146`、`objectFallbackDrawCount=0`。
     - 当前返回 `hot-shadow-render-scene-pending`，原因是 isolated desktop 尾帧 scene 消费没追上；summary 中 core packet 已完成，不是上游数据断。
   - `dynamic_shadow_pressure`：
     - 工件：`AutoTest/artifacts/codex_dynamic_shadow_pressure_after_attachment_precheck_20260425.json`
     - `ok=true`、`semanticCoreBuildDurationUs=7067`、`semanticCoreSkinnedResolved=30`、`semanticSceneSubmitted=30`、`objectFallbackDrawCount=0`。
   - trusted runtime roots 追加验证：
     - 工件：`AutoTest/artifacts/codex_dynamic_shadow_pressure_trusted_runtime_roots_20260425.json`
     - `ok=true`、`semanticCoreBuildDurationUs=143`、`semanticCoreSlowestRecordResolveUs=25`、`semanticCoreSlowestPoseRuntimeRootsUs=0`、`semanticCoreSkinnedResolved=30`、`objectFallbackDrawCount=0`。
     - 低压图补充工件：`AutoTest/artifacts/codex_low_pressure_trusted_runtime_roots_20260425.json`，单条解析慢点已压平，但 isolated desktop 尾帧时 `buildInProgress=true`、`buildCurrentRecordIndex=64/141`，说明下一步是 build drain/single-flight，不是继续追单条 owner/pose 慢点。
3. 硬结论
   - 用户看到的 5 FPS / 未响应主因不是“模型数据拿不到”，而是 consumer 侧两个全表兜底：
     - `findRuntimeModelOwner` 每条记录扫全量 runtime model；
     - attachment rigid 兜底对普通记录也扫附件表。
   - 当前这两个热点已被压下：标准场景 semantic core build 已从约 676ms 降到 143us 级别；低压图单条解析慢点已压平，剩余是 chunked build 在尾帧没 drain 完。
   - 低压图仍可能出现 isolated desktop 尾帧 `runtimeFrameAdvanceStalled=true` / scene pending；这需要继续和地图脚本锁视角、AutoTest tail-frame gate 分开看。
4. 当前 blocker
   - 还没有完成真正 FPS 报告闭环：当前 isolated desktop/windowed perf report 仍可能 `avgFps/frameCount=null`。
   - 低压图 consumer 总耗时仍有约 311ms/8s 样本，下一轮优先继续做：
     - semantic build single-flight / revision 去重；
     - contract capture 重复发布计数；
     - runtimeRoots 兜底结果缓存或跳过策略。
   - 不再重开 owner/resource/palette 逆向；当前主线是消费层去重和性能。

#### 9.11.30 2026-04-25 semantic build tail-frame drain + FPS 采样口径

1. 完成内容
   - `ShadowValidationRuntime` 新增 bounded control-plane drain：
     - `drainPendingBuildForControlPlane(maxChunks, maxTotalBudgetUs, recordCeiling)`。
     - 只在 `get_shadow_runtime_summary/get_hot_shadow_probe` 显式 `refreshSemanticFrameIfStale` 且 semantic preview scene submission 打开时生效。
     - 当前上限：每次最多 6 个 chunk、12ms、2048 records。
   - 目的不是把完整 build 搬回 pipe 线程，而是处理 isolated desktop/windowed 尾帧不再推进真实 render tick 时，低压图 chunked build 永久停在 `buildInProgress=true` 的情况。
   - AutoTest summary 增加 FPS 样本可靠性字段：
     - `fpsSampleReliable`
     - `fpsSampleFrameCount`
     - `fpsSampleWindowSec`
     - `runtimeFrameDeltaReadyToHotShadow`
     - `fpsSampleReliabilityReason`
   - 这样 `frameCount=1/2` 的隔离桌面 perf report 不再被误当作真实性能结论。

2. Fresh 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `git diff --check` 通过，仅有既有 CRLF 提示。
   - 残留进程检查：无 War3 / Warcraft / Frozen / World Editor 进程。

3. 场景结果
   - `model_runtime_probe`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260425_063412.png`
     - 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_34_15.html`
     - `ok=true`
     - `semanticCoreBuildInProgress=false`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreBuildDurationUs=135`
     - `semanticCoreSkinnedResolved=30`
     - `semanticCoreSubmittedDrawCount=32`
     - `semanticSceneSubmittedSkinned=30`
     - `objectFallbackDrawCount=0`
   - `光影测试低压.w3x / low_pressure_static_reuse`
     - 使用地图：`E:\Work\War3\Maps\ShadowTest\光影测试低压.w3x`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260425_063521.png`
     - 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_35_20.html`
     - `ok=true`
     - ready 时曾为 `semanticCoreBuildInProgress=true / currentIndex=46/138`。
     - hot-shadow 后成功 drain：`semanticCoreBuildInProgress=false`
     - `semanticCoreBuildDurationUs=54293`
     - `semanticCoreSkinnedResolved=137`
     - `semanticCoreSubmittedDrawCount=143`
     - `semanticSceneSubmittedSkinned=157`
     - `objectFallbackDrawCount=0`
     - `semanticTailSceneAccepted=true`
   - `dynamic_shadow_pressure`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260425_063612.png`
     - 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_36_15.html`
     - `ok=true`
     - `semanticCoreBuildInProgress=false`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreBuildDurationUs=115`
     - `semanticCoreSkinnedResolved=30`
     - `semanticCoreSubmittedDrawCount=32`
     - `semanticSceneSubmittedSkinned=30`
     - `objectFallbackDrawCount=0`

4. 硬结论
   - 低压图之前的 `buildInProgress=true/currentIndex=64/141` blocker 已被修掉；现在 summary refresh 可以 bounded drain 到完整 semantic frame。
   - 三场景都能产生 semantic skinned scene submission，且 control-plane runtime summary 中 object fallback 为 0。
   - 当前 AutoTest perf HTML 的 `avgFps≈0` 不代表真实游戏性能：隔离桌面 + windowed 下 report 只记录 1-2 个 Present frame，frame 跨过了等待/截图/尾帧阶段。后续必须看 `fpsSampleReliable`；若为 false，不能把 avgFps 当性能结论。

5. 当前 blocker
   - correctness gate 已恢复，下一步转回真正性能：
     - 若要拿真实 FPS，需做可见桌面专用 perf run 或新增 control-plane perf reset/export 后的可见帧采样。
     - 语义侧下一刀是减少低压图 `semanticConsumerBuildUs≈683ms` 和 `semanticContractCaptureUs≈90ms` 的累计成本，优先做 manifest revision single-flight / contract capture skip counter。
- 不再重开 owner/resource/palette 逆向。

#### 9.11.31 2026-04-25 semantic single-flight 清理 + contract capture no-op skip

1. 完成内容
   - `ShadowValidationRuntime` 新增 stale pending build 清理：
     - 当当前 manifest/revision 已经完成 build，且 pending manifest 不比已完成 frame 更新时，直接清空 pending snapshot。
     - 新增 `semanticCoreStalePendingBuildClearedCount`，用于确认不是又卡在旧 pending request。
   - `ShadowRuntimeContractCache::captureLiveState` 增加同帧 no-op skip：
     - 同一 frameSerial 下 pose/attachment 计数没有增长且已有可用 contract 时，直接跳过完整 snapshot。
     - 新增三个 skip counter：`semanticContractCaptureSkippedStableSameFrame`、`semanticContractCaptureSkippedEmpty`、`semanticContractCaptureSkippedDuplicateSameFrame`。
   - `get_shadow_runtime_summary` 已暴露上述 contract/core counters。
   - AutoTest 增加 near-latest tail-frame 接受逻辑：
     - 若 core 已被 control-plane drain 到最新 frame，但 isolated desktop render scene 只落后一帧且已有 semantic scene submission，不再误判为数据链失败。
     - 注意：当前运行中的 MCP server 可能仍是旧代码，需重启/重载 MCP 后才会实际使用 `semanticTailSceneNearLatestAccepted`。

2. Fresh 验证
   - `ninja -C build32 -j4` 通过（no work to do）。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `git diff --check` 无 whitespace error，仅输出既有 LF/CRLF 提示。
   - 残留进程检查：无 War3 / Warcraft / Frozen / World Editor 进程。

3. 场景结果
   - `光影测试低压.w3x / low_pressure_static_reuse`
     - 最新 summary：`semanticCoreBuildInProgress=false`、`semanticCoreBuildRequestPending=false`、`semanticCoreFrameFresh=true`。
     - `semanticCoreFrameSerial=149`、`semanticSceneLastFrameSerial=148`、`semanticScenePublishRevisionLag=3`。
     - `semanticCoreSubmittedDrawCount=158`、`semanticCoreSkinnedResolved=144`、`semanticCoreAttachmentRigidResolved=6`。
     - `semanticSceneSubmittedSkinned=139`、`objectFallbackDrawCount=0`。
     - `semanticCoreStalePendingBuildClearedCount=5`。
     - `semanticContractCaptureSkippedStableSameFrame=8`、`semanticContractCaptureSkippedDuplicateSameFrame=17`。
     - `semanticContractCaptureUs=69915`，相比前一轮低压样本约 `116649us`/更早 `179ms` 明显下降。
     - 当前 AutoTest 仍返回 `hot-shadow-render-scene-pending`，但原因是 isolated desktop scene consumption 落后一帧；core/contract/object fallback 都已满足，不是上层数据链失败。
   - `dynamic_shadow_pressure`
     - hot-shadow summary 通过：`semanticCoreBuildInProgress=false`、`semanticCoreFrameFresh=true`、`semanticCoreBuildDurationUs=136`。
     - `semanticCoreSkinnedResolved=30`、`semanticCoreSubmittedDrawCount=32`、`semanticSceneSubmittedSkinned=30`、`objectFallbackDrawCount=0`。
     - `semanticContractCaptureSkippedStableSameFrame=3`、`semanticContractCaptureSkippedDuplicateSameFrame=17`。
     - 这一轮最终 tool `ok=false` 只因为截图/control-plane capture width/height=0 且 perf report 没刷出新文件；runtime correctness 已通过。
   - 复用上一轮 fresh visual 工件作为当前视觉基线：
     - `AutoTest/artifacts/screenshots/war3_20260425_065159.png`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_52_03.html`
     - `AutoTest/artifacts/screenshots/war3_20260425_065255.png`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_52_58.html`

4. 硬结论
   - 旧 pending request 已不会长期挂住：latest summary 中 `semanticCoreBuildRequestPending=false`，且 stale clear counter 会增长。
   - contract capture 的重复工作已经可观测、可跳过；低压图 contract capture 累计耗时已被压下一档。
   - 当前剩余问题不是“上层数据拿不到”，而是：
     - isolated desktop 下 AutoTest render-scene tail gate 需要重载 MCP 才能使用新的 near-latest 接受逻辑；
     - 低压图 `semanticConsumerBuildUs` 仍约 `208ms`，还要继续压 consumer/build 累计成本；
     - isolated desktop/windowed perf report 仍常见 `frameCount=1`，不能用其 `avgFps` 当真实性能结论。

5. 下一轮精确入口
   - 先重启/重载 AutoTest MCP server，复跑 `光影测试低压.w3x`，确认 `semanticTailSceneNearLatestAccepted=true` 能把 scene tail 从失败降级为 accepted。
   - 然后继续压 `semanticConsumerBuildUs`：
     - 优先看 `semanticLastHotFunctionTag=6` / `semanticLastHotFunctionUs≈15917` 对应的 consumer 子路径；
     - 继续做 manifest revision single-flight 与 build result 复用；
     - 只对 missing/unchanged manifest 做 cooldown，不要恢复旧 snapshot/freeze/VB/IB。
   - 真实 FPS 需要单独设计可见桌面 perf run 或 control-plane perf reset/export；不要用 isolated desktop 1-frame report 下结论。

#### 9.11.32 2026-04-25 AutoTest semantic validation env 显式化

1. 新发现
   - 9.11.31 后直接跑 `model_runtime_probe` 时，ready 能通过，但 hot-shadow 一直失败。
   - summary 里 `semanticBuildSkippedReason=6`，对应 `SceneSubmissionDisabled`。
   - 根因不是 C++ 数据链回退，而是前一轮为修复手动进图白屏/未响应，把 semantic scene submission 改成运行期必须显式 env 打开；AutoTest named scenario 预设没有同步设置这些 env。

2. 修复内容
   - `AutoTest/war3_autotest_mcp.py` 新增 `SEMANTIC_SHADOW_VALIDATION_ENV`：
     - `DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK=1`
   - `low_pressure_static_reuse`、`dynamic_shadow_pressure`、`model_runtime_probe`、`semantic_cost_probe` 预设都会默认带这组 env。
   - `run_named_scenario` 改为先合并 preset env，再叠加调用方 `env_overrides_json`；调用方仍可覆盖具体值。
   - 普通手动进图默认仍保持 semantic preview 关闭，不会复发“默认实验 consumer 把手动路径拖白屏/未响应”。

3. Fresh 验证
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - 由于当前 MCP server 未重载，立即复测时仍需手动传同一组 `env_overrides_json` 才能生效；重载 MCP 后 named preset 会自动带 env。

4. 显式 env 复测结果
   - `model_runtime_probe`
     - ready：`elapsedSec=6.246`。
     - hot-shadow：`ok=true`。
     - `semanticBuildSkippedReason=0`。
     - `semanticCoreBuildDurationUs=135`。
     - `semanticCoreSkinnedResolved=30`。
     - `semanticSceneSubmittedSkinned=30`。
     - `semanticCoreSubmittedDrawCount=32`。
     - `objectFallbackDrawCount=0`。
     - 总 tool `ok=false` 仅因为截图/control-plane capture width/height=0 且 perf report 没写出新文件；核心语义阴影正确性已通过。
   - `low_pressure_static_reuse / 光影测试低压.w3x`
     - ready：`elapsedSec=7.321`。
     - `semanticBuildSkippedReason=0`。
     - `semanticCoreFrameFresh=true`。
     - `semanticCoreBuildInProgress=false`。
     - `semanticCoreBuildDurationUs=46958`。
     - `semanticCoreSkinnedResolved=138`。
     - `semanticSceneSubmittedSkinned=146`。
     - `semanticSceneSubmitted=146`。
     - `objectFallbackDrawCount=0`。
     - `semanticContractCaptureUs=58862`。
     - `semanticConsumerBuildUs=164037`。
     - 当前 old MCP gate 仍返回 `hot-shadow-skinned-pending`，但 summary 已满足 semantic scene same-frame consumption；重载 MCP 后应由 `semanticTailSceneNearLatestAccepted` 或同帧 scene gate 接受。

5. 当前 blocker 更新
   - AutoTest tooling 层：
     - 重载 MCP server 后再跑 named scenario，不要继续用旧 server 判断新 gate。
     - 截图失败集中在 isolated desktop 的 fallback window capture `WindowRect invalid: 0x0`，但候选窗口本身有 1902x963；这是截图脚本在隔离桌面 window rect 读取上的后处理问题。
     - perf report 仍可能不产出新 runtime benchmark，需要单独修 report/export 或做可见桌面 perf。
   - semantic 性能层：
     - 低压图显式 env 下 consumer 累计从上一轮约 `208ms` 降到 `164ms`，contract capture 从约 `70ms` 降到 `59ms`，方向正确但仍需继续压。
     - 下一刀继续拆 `semanticLastHotFunctionTag=6`，不要回头改 owner/resource/palette 逆向。

#### 9.11.33 2026-04-25 AutoTest same-frame semantic scene gate 收口

1. 本轮修复
   - `AutoTest/war3_autotest_mcp.py` 的第二条 control-plane hot-shadow 判定链补齐 same-frame semantic scene acceptance。
   - 新规则：只要 semantic scene 已提交并被消费、manifest fresh、attachment gate 满足、`objectFallbackDrawCount=0`，即使 `wait_until` 把最终状态分类成 pending/stalled，也允许把该轮 hot-shadow 判为 correctness accepted。
   - 这正对 9.11.32 的低压图读数：
     - `semanticCoreSkinnedResolved=138`
     - `semanticSceneSubmittedSkinned=146`
     - `semanticSceneSubmitted=146`
     - `semanticSceneConsumptionFresh=true`
     - `semanticSceneSameFrameConsumed=true`
     - `objectFallbackDrawCount=0`
   - 换句话说：低压图这轮不是“没有语义阴影”，而是旧 AutoTest gate 仍把已经消费过的 semantic scene 当成 `hot-shadow-skinned-pending`。

2. Fresh 验证
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `ninja -C build32 -j4` 通过（no work to do）。
   - `git diff --check` 无 whitespace error，仅输出既有 LF/CRLF 提示。
   - `mcp__war3_autotest__.current_state` 显示当前无 War3 进程，`debugMonitorRunning=true`。

3. 本轮没有重跑低压图的原因
   - 当前 MCP server 很可能仍是旧 Python 进程，未必会加载刚修改的 gate/preset。
   - 旧 `low_pressure_static_reuse` preset 在历史版本里曾经是非 windowed/fullscreen 风险；在确认 MCP 重载前不盲跑，避免再次影响前台或触发黑屏/雪花屏。
   - 下一轮第一步必须先重载/重启 AutoTest MCP，再跑 named scenario；重载后低压图应由 `semanticSceneOnlyAccepted=true` 或 tail scene accepted path 通过。

4. 当前阶段判断
   - correctness 侧：上层 semantic data 到 DXVK validation scene 已经能产出 skinned semantic packets，fallback 为 0。
   - tooling 侧：AutoTest gate/preset 已修，但需要 MCP 重载后才能作为自动验收依据。
   - 性能侧：主 blocker 仍是低压图 `semanticConsumerBuildUs≈164ms` 级别累计成本，重点继续看 `semanticLastHotFunctionTag=6` / ConsumerBuild。

5. 下一轮精确入口
   - 重载 AutoTest MCP，先跑 `model_runtime_probe`，再跑 `low_pressure_static_reuse`。
   - 若 named scenario 仍失败，优先检查返回里是否有 `semanticSceneOnlyAccepted`、`semanticTailSceneAccepted`、`semanticTailSceneNearLatestAccepted`，再判断是否是真失败。
   - 通过 gate 后继续优化 consumer build：
     - 继续做 `frameSerial + manifestRevision` single-flight。
     - 复用 last completed semantic frame，禁止缺数据 catchup 循环。
     - 不恢复 snapshot/freeze/VB/IB 主路径，不重开 owner/resource/palette 逆向。

#### 9.11.34 2026-04-25 semantic consumer 空 miss build 收口与新 blocker

1. 本轮改动
   - `ShadowRendererCore` 的 preview cap 现在会把 owner resource / inferred geosetIndex 写回预览记录副本，避免“打分命中、resolve 仍 miss”的字段断层。
   - preview score 规则修正：`ObjectKind::Unit` 但没有 ready resource 的记录不再打成 `<11`，避免 256 条匿名 unit resource-miss 挤掉真正 ready 的 semantic 记录。
   - 当 preview manifest 没有任何 ready record 时，core 不再全量啃 429 条必 miss 记录，而是快速提交空预览帧；同时加入一个保守的 miss-only build supersede gate，只有当前 build 已处理 64 条且 0 resolved / 全 resource miss 时，才允许更新 manifest 抢占。
   - 增加了轻量 `PoseStore + ResourceStore` seed 尝试，但当前 live 结果显示 seed 没有生成可用 skinned record，不能把它当成完成路径。

2. Fresh artifact
   - `AutoTest/artifacts/codex_preview_owner_index_infer_model_probe.json`
   - `AutoTest/artifacts/codex_ready_score_only_model_probe.json`
   - `AutoTest/artifacts/codex_skip_no_ready_preview_model_probe.json`
   - `AutoTest/artifacts/codex_pose_resource_seeds_model_probe.json`
   - `AutoTest/artifacts/codex_low_pressure_after_preview_ready_score.json`

3. 新硬结论
   - consumer 单条解析成本已压平：`codex_preview_owner_index_infer_model_probe` 中 `semanticConsumerBuildUs=64999`、`semanticCoreSlowestRecordResolveUs=518`，不再是 2 秒级单条卡死。
   - 真正导致 semantic-only 不出图的新 blocker 是 contract 输入没有 runtime/modelResource 绑定：
     - 低压图 ready/hot summary：`recordsWithRuntimeModel=0`、`recordsWithModelResource=0`、`recordsWithResolvedGeoset=106`。
     - `rootUnitSupplementSeedCount=763`，其中 `rootUnitSupplementSkippedNoResource=735`，说明 pose/identity seed 已有，但无法命中 runtime/model resource。
     - `shadowReadyGeosetCount=109`、`shadowRuntimeModelCount=72` 仍存在，问题不是“完全没资源”，而是 visible/pose seed 与 runtime resource owner 没有对上。
   - 当前低压图仍有 `objectFallbackDrawCount=71`，所以可见阴影仍主要来自旧 fallback；semantic-only skinned scene 当前没有通过。

4. 当前 blocker
   - 需要修 `runtimeModel/pose seed -> ShadowModelResourceCache runtime owner/resource` 的绑定，而不是继续优化 receiver/AA/DXVK pass。
   - 优先怀疑点：
     - `noteRuntimeGeosetBinding` 只有 `geosetIndex` 有效时才把 runtime model 记录写完整；当前 visible records 多数只有 `runtimeGeosetDataPtr`，`geosetIndex=0xFFFFFFFF`。
     - `AppendRootUnitSupplementRecords` 能拿到大量 seed，但 `TryFindRuntimeModelResourceForSeed` 找不到 ready runtime/modelResource。
   - 下一轮精确入口：
     - 在 `ShadowModelResourceCache::noteRuntimeGeosetBinding` / `findRuntimeModelOwner` 增加“runtimeGeosetData -> owner runtime/modelResource/index”诊断计数。
     - 追为什么 `rootUnitSupplementSkippedNoResource` 高达 735：是 seed runtimeModel 没进入 `m_byRuntimeModel`，还是 runtime record readyGeosetCount 为 0。
     - 不重开 `WorldObjectList+0x14` / `a3/sourceObject` / sourceObject+0x100；也不恢复 snapshot/freeze/VB/IB 主路径。

#### 9.11.35 2026-04-25 root supplement resource/no-geoset 分桶与热帧安全收口

1. 本轮改动
   - `AppendRootUnitSupplementRecords` 增加资源 miss 分桶：
     - `rootUnitSupplementResourceCacheMiss`
     - `rootUnitSupplementResourceCacheNotReady`
     - `rootUnitSupplementResourceSemanticKeyResolved`
     - `rootUnitSupplementResourceSemanticKeyReady`
   - root supplement 在 cache miss 时允许用既有 `TryResolveRuntimeModelSemanticKey` 做小预算反查，但预算固定收缩为每次 supplement 最多 8 个唯一 runtime alias，避免低压图 seed 全量深解析。
   - no-geoset 再拆成：
     - `rootUnitSupplementSkippedNoGeosetZeroCount`
     - `rootUnitSupplementSkippedNoGeosetStoreMiss`
     - `rootUnitSupplementSkippedNoGeosetNotReady`
   - supplement 查 resource 时补了 `runtimeResource.geosetPtrs/geosetDataPtrs -> ShadowModelResourceStore` 的 fallback，避免只靠 runtimeModel/modelResource alias。
   - 曾尝试在 store-miss 后热帧全量 rebuild resource store，但低压图 control-plane 直接超时，已收回；结论是不能在热帧里用全量 rebuild 修这个问题。

2. Fresh artifact
   - `AutoTest/artifacts/codex_resource_semantic_key_model_probe.json`
   - `AutoTest/artifacts/codex_resource_semantic_key_budget_low_pressure.json`
   - `AutoTest/artifacts/codex_resource_geoset_fallback_low_pressure.json`
   - `AutoTest/artifacts/codex_no_geoset_breakdown_low_pressure.json`
   - `AutoTest/artifacts/codex_refresh_store_on_supplement_miss_low_pressure.json`（失败样本：热帧全量 rebuild 导致 pipe 超时）

3. 新硬结论
   - resource miss 主因已大幅缩小：低压图从上一轮 `rootUnitSupplementSkippedNoResource=735` 降到约 `46~54`。
   - semantic key 反查有效但必须限流：低压图可见 `rootUnitSupplementResourceSemanticKeyResolved=19~26`、`Ready=19~26`；不加预算会拖垮 control-plane。
   - 当前新 blocker 已转为 resource store 快照/alias 问题：
     - `rootUnitSupplementSkippedNoGeoset=849`
     - `rootUnitSupplementSkippedNoGeosetZeroCount=0`
     - `rootUnitSupplementSkippedNoGeosetStoreMiss=848`
     - `rootUnitSupplementSkippedNoGeosetNotReady=0`
   - 这说明 runtimeResource 本身有 geosetCount，也不是 geoset 不 ready，而是发布给 consumer 的 `ShadowModelResourceStore` 查不到这些 geoset/alias。
   - 热帧全量 rebuild store 已证伪：`codex_refresh_store_on_supplement_miss_low_pressure` 直接 `等待 control-plane 响应超时`，不能作为修复路线。

4. 当前 blocker
   - 下一步不要继续扩大 semantic key 深解析，也不要在 consumer/control-plane 热路径全量 rebuild resource store。
   - 精确入口改为：
     - `ShadowModelResourceStore` 增量 alias/增量 add：只把 supplement 需要的 missing geoset ptr/data 补进 store，而不是 snapshot 全量 rebuild。
     - 或者在 `ShadowModelResourceCache` 写入阶段让 runtime/model alias 与 geoset store 同帧轻量同步，避免 `resourceRefreshCoolingDown` 复用旧 store 时漏掉新 runtime geoset。
   - 当前 correctness 仍未完成：低压图 `semanticSceneSubmitted=4`、`semanticSceneSubmittedSkinned=0`、`objectFallbackDrawCount=0`，说明 semantic rigid/少量提交存在，但蒙皮 semantic-only 仍没闭环。

#### 9.11.36 2026-04-25 root supplement geoset overlay 与 core store-miss 消费修复

1. 本轮改动
   - `AppendRootUnitSupplementRecords` 在 `ShadowModelResourceStore` 查不到 geoset 时，改为用小预算单条 `ShadowModelResourceCache` fallback 取当前 geoset，不再做热帧全量 rebuild。
   - 新增 `rootUnitSupplementGeosetCacheFallback` control-plane counter，用来区分“store miss 但 cache 可直接补单条 geoset”的命中量。
   - root/unit supplement 的 duplicate 判定改成：有 `runtimeModelPtr` 时只按 `runtimeModel + geoset` 去重；只有 runtime 不明时才退回 `modelResource + geoset` 去重，避免多个单位共用同一模型时被错误压成一条。
   - `ShadowRendererCore` 在 store miss 时也允许按 renderable record 的 `runtimeGeosetPtr/runtimeGeosetDataPtr/runtimeModel+geoset/modelResource+geoset` 从 `ShadowModelResourceCache` 拉取单条资源，避免 manifest 里有 resolved geoset 但 core 仍 resource-miss。
   - supplemented manifest 现在要求 semantic core 的 `sourcePublishRevision` 精确追上；不能再用 `publishRevisionLag<=16` 把补充过的 revision 当成 fresh。
   - preview cap 在 ready records 少于 32 条时继续追加 pose/resource seeds，避免 overlay ready record 抢占后把旧的 skinned seed 路线完全挤掉。

2. Fresh artifact
   - `AutoTest/artifacts/codex_incremental_geoset_overlay_model_probe.json`
   - `AutoTest/artifacts/codex_incremental_geoset_overlay_dedupe_low_pressure.json`
   - `AutoTest/artifacts/codex_core_cache_fallback_dynamic_pressure.json`
   - `AutoTest/artifacts/codex_core_cache_fallback_low_pressure.json`
   - `AutoTest/artifacts/codex_core_cache_fallback_pose_seed_dynamic_pressure.json`

3. 新硬结论
   - 低压图的 `rootUnitSupplementSkippedNoGeosetStoreMiss` 已从上一轮约 `848` 降到 `18` 级别，说明“轻量单条 geoset overlay”方向有效。
   - `rootUnitSupplementGeosetCacheFallback` 在低压图可见 `60`，`rootUnitSupplementAppended=32`，说明 store miss 不是几何缺失，而是 store/alias 发布层漏索引。
   - `dynamic_shadow_pressure` 在 `codex_core_cache_fallback_dynamic_pressure.json` 中已经恢复到 core 有提交：`semanticCoreSubmittedDrawCount=6`、`semanticCoreSkinnedResolved=2`、`objectFallbackDrawCount=0`。这证明 core-side cache fallback 能消费 overlay geoset。
   - 仍不能把本轮判为完成：latest dynamic/low-pressure 在 isolated desktop 下仍可能停在 `semanticCoreBuildRequestPending=true` 或 render-scene 未消费 latest revision，AutoTest 总结果仍是 hot-shadow pending/stalled。
   - 曾尝试在 control-plane 对 pending build 做 6 chunk bounded drain，但会复现 `等待 control-plane 响应超时`，已收回；这条路线不能直接保留。

4. 当前 blocker
   - 数据层 blocker 已从“大量 geoset store miss”推进到“pending semantic build/scene consumption timing”：补充记录已经进 manifest，core 也能按 cache fallback 消费，但 isolated desktop 尾帧经常没有足够 render tick 去完成/提交 latest revision。
   - 下一轮精确入口：
     - 不要再扩大 semantic key budget，也不要恢复热帧全量 rebuild。
     - 专门做安全的 pending build 完成策略：优先考虑在 render thread 更早消费 pending build，或在 request 阶段避免把未消费 raw revision 判为 fresh；不要从 control-plane 同步 drain 大块工作。
     - 继续观察 `semanticCoreBuildRequestPending`、`semanticCoreSourcePublishRevision` vs `semanticCoreManifestPublishRevision`、`semanticSceneSubmittedSkinned`、`objectFallbackDrawCount`。

#### 9.11.37 2026-04-25 indexed runtime-owner hydrate 与隔离桌面低压验证

1. 本轮改动
   - `ShadowModelResourceCache` 新增 `findRuntimeModelOwnerIndexed(...)`，只查已有 `runtimeGeoset/runtimeGeosetData -> runtimeModel` 索引，不进入旧 `findRuntimeModelOwner(...)` 的全量 runtime 扫描。
   - `ShadowRuntimeContractCache::captureLiveState()` 在 consumer build 前新增 `HydrateManifestRuntimeOwnersFromIndexedCache(...)`，把 manifest 中只有 `runtimeGeoset/Data` 的记录轻量反填 `runtimeModelPtr / modelResourcePtr / modelKey`。
   - 目标是补 direct pose key 覆盖，而不是把昂贵 owner/pose fallback 扫描重新放回 `ShadowRendererCore`。

2. Fresh artifact
   - `AutoTest/artifacts/codex_low_pressure_indexed_owner_hydrate_20260425.json`
   - `AutoTest/artifacts/screenshots/codex_low_pressure_indexed_owner_hydrate_20260425.png`
   - 最新性能报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_17_43_35.html`

3. 新硬结论
   - 隔离桌面 + windowed + `光影测试低压.w3x` 下编译与启动通过，control-plane summary 成功返回。
   - indexed owner hydrate 有效：相对上一轮 strict direct-pose 样本，`semanticCoreSkinnedResolved` 从 `66` 提升到 `84`，`semanticCoreSkippedNoPose` 从 `62` 降到 `44`。
   - 计算量没有反弹：`semanticCoreBuildDurationUs=11400`，`semanticCoreSlowestRecordResolveUs=347`，`semanticCoreSlowestResourceLookupUs=0`，`semanticCoreSlowestPoseResolveUs=2`，说明没有回到秒级 resource/pose fallback storm。
   - 当前 semantic-only 正确性仍未完成：`nativeD3D9BackendSubmittedSkinnedDrawCount=84`，但 `nativeD3D9BackendExecuteAttemptCount=0`；截图仍偏暗且 isolated desktop perf report 只有 `frameCount=3`，不能作为最终 FPS 结论。

4. 当前 blocker
   - 数据消费热路径的计算量路线已经闭环：继续补 direct pose 覆盖要在 producer/contract 侧做 O(1) key 发布，不允许恢复 consumer 侧全量 registry/resource runtime tree 扫描。
   - 下一轮精确入口：
    - 补剩余 `semanticCoreSkippedNoPose=44` 的 direct pose 来源，优先看 manifest 记录是否已有 runtimeModel 但 `TryReadLiveCModelPose` 失败，还是仍缺 runtimeModel owner index。
    - 修 semantic scene/native backend 执行时机：当前 submitted skinned packet 已有，但 execute counter 为 0，不能把“submitted”误判为“实际画到了屏幕”。
    - 视觉阶段需要继续排查画面偏暗/色温失效，但先保证 semantic scene 实际 execute，再做亮度色温。

#### 9.11.38 2026-04-25 semantic.data 卡顿二分：VisibleRenderableRegistry finalizer

1. 本轮改动
   - 新增 semantic.data 二分开关：
     - `kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites`
     - `kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites`
     - `kWar3RuntimeConfigDisableSemanticVisibleFinalizeIdentityResolve`
     - `kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata`
     - `kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata`
     - `kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery`
   - `VisibleRenderableRegistry` 支持只写轻量 visible records，跳过 `FinalizeVisibleRecord()` 的热路径补全。
   - 当前临时安全配置：`FrameRegistries=true`、visible writes 保留、lightweight visible writes 开启，用于避免继续触发 5 FPS 级卡顿。

2. Fresh artifact
   - `AutoTest/artifacts/codex_manual_data_only_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_manual_no_shadow_no_semantic_20260425.json`
   - `AutoTest/artifacts/codex_manual_model_hooks_off_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_manual_geoset_capture_off_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_manual_spriteframe_hooks_off_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_manual_frame_reg_off_model_on_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_visible_manifest_writes_off_frame_reg_on_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_visible_manifest_lightwrite_frame_reg_on_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_visible_finalize_no_modelmetadata_shadow_off_20260425.json`
   - `AutoTest/artifacts/codex_visible_finalize_all_heavy_off_shadow_off_20260425.json`

3. 新硬结论
   - 关掉 shadow/postfx/AA/semantic consumer 但保留 semantic.data 仍只有约 `8.6 FPS`，说明卡顿不是 receiver/postfx/AA，也不是 semantic draw consumer。
   - `semantic.data` 全关可恢复到约 `218 FPS`；`SemanticModelHooks` 关掉也可恢复到约 `204 FPS`。
   - 单独关闭 geoset resource capture 后仍约 `8.8 FPS`；关闭 SpriteFrameUpdate 系列 Hook 后仍约 `10.9 FPS`，所以主因不是 geoset copy，也不是 pose hook。
   - `FrameRegistries=false` 且 model hooks 保留时恢复到约 `212 FPS`，主因收敛到 frame registry 路线。
   - `FrameRegistries=true` 但禁用 visible manifest 写入时约 `215 FPS`；说明 begin/end 生命周期不是主因。
   - visible manifest 轻量写入 109 条仍有约 `177 FPS`；说明数组/hash map 写入本身不是灾难。
   - 只禁用 `ResolveModelMetadata()` 仍约 `14 FPS`；禁用 identity/model/geoset/sibling heavy stages 仍约 `22 FPS`；完整跳过 `FinalizeVisibleRecord()` 才恢复。这证明瓶颈在 `VisibleRenderableRegistry::FinalizeVisibleRecord()` 这整个 per-record 热路径，尤其入口 `TryReadSceneNodeFromRenderablePart/TryReadMeshDataFromRenderablePart` 触发的 `SafeRead*Fast`。
   - `SafeRead*Fast` 当前实现实际每次调用 `VirtualQuery`（缓存被临时禁用），因此在 visible renderable per-record 热路径中会形成 VirtualQuery storm。

4. 当前 blocker
   - 不能把 `FinalizeVisibleRecord()` 放在 `registerMainQueueRange/registerTransparentEntry/registerSemanticCandidate` 热路径里逐条执行。
   - 下一轮精确入口：
     - 保留 hot path 只做 raw visible record O(1) append。
     - 把 sceneNode/meshData/runtimeModel/geoset/identity/resource 补全移到 end-frame 或 contract capture 的去重批处理阶段。
   - 对补全按 `renderablePart/sceneNode/runtimeGeosetData/runtimeModel` 建 per-frame cache，保证每个 unique key 最多一次 `SafeRead*Fast/VirtualQuery`。
   - 重新评估 `IsReadableRangeFast` 的 TLS region cache；若恢复全局缓存风险高，则至少在 visible manifest hydrate 层做局部缓存，避免重复 `VirtualQuery`。
   - 当前临时配置偏性能诊断，不代表最终 semantic-only 完成；下一轮需要在不恢复 hot-path finalizer 的前提下重新补齐 semantic skinned/resource/geoset contract。

#### 9.11.39 2026-04-25 visible manifest 第一刀修复：帧末基础 hydrate

1. 本轮改动
   - 保持 `VisibleRenderableRegistry::appendRecord()` 热路径为轻量 raw record 写入，不恢复 `FinalizeVisibleRecord()`。
   - 新增 `kWar3RuntimeConfigSemanticVisibleEndFrameBasicHydrate=true`：在 `endFrame()` 对本帧 visible records 做一次基础补全。
   - 新增 `kWar3RuntimeConfigTrustVisibleRenderablePartPointers=true`：对 War3 本帧 render queue 正在消费的 `renderablePart` 固定槽位直读 `sceneNode/meshData`，避免每条 visible record 触发 `SafeReadPtrFast -> VirtualQuery`。
   - `endFrame()` hydrate 后重建 visible indexes，使 `bySceneNode/byMeshData` 可用于后续 contract/consumer，但不把 identity/model/geoset 多级恢复放回热路径。

2. Fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 验证尝试：`AutoTest/artifacts/codex_visible_basic_hydrate_data_only_20260425.json` 本轮未生成，原因是隔离桌面启动后未进图，`runtime_status.json` 停在 `gameStarted=false / inGameRenderReady=false`。

3. 新硬结论
   - 已确认 `war3_memory.cpp::IsReadableRangeFast()` 当前仍是单次 `VirtualQuery` 实现，没有启用原注释提到的 TLS region cache。
   - 因此 visible finalizer 卡顿不是抽象猜测：`TryReadSceneNodeFromRenderablePart/TryReadMeshDataFromRenderablePart` 的 safe-read 在 per-record 热路径里会制造 VirtualQuery storm。
   - 本轮选择局部直读 + 帧末 hydrate，而不是全局恢复 `IsReadableRangeFast` 缓存；原因是全局缓存曾因崩溃诊断被关闭，直接恢复会影响所有 SafeRead 调用面，风险更大。

4. 当前 blocker
   - 下一轮需要重新跑隔离桌面低压验证，确认：
     - FPS 是否保持在 lightweight visible writes 的高位，而不是回到 5~22 FPS；
     - `visibleRenderableCount` 是否保留；
     - `bySceneNode/byMeshData` 索引是否因帧末 hydrate 恢复；
     - 后续 contract 能否在不恢复 hot-path finalizer 的前提下恢复 `semanticCoreSkinnedResolved / semanticSceneSubmittedSkinned`。
   - 如果本轮 ready 失败复现，先排 AutoTest/启动链，不要把它误判为 semantic.data 性能回退。

#### 9.11.40 2026-04-25 render-off semantic.data 性能收口

1. 本轮改动
   - `kWar3RuntimeConfigDisableSemanticGeosetResourceCapture=true` 时，不再安装 `CreateGeosetFromRawArrays` hook。
   - `Hook_CreateGeosetFromRawArrays` 内部也保留同一编译期开关早退，避免未来误装后继续记 perf counter。
   - 延续上一轮 visible manifest 修复：`render` 模块关闭时 visible manifest 不发布，frame registry publish 保持 0。

2. Fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - `AutoTest/artifacts/codex_render_off_semantic_on_no_geoset_hook_perfonly2_20260425.json`
   - `AutoTest/artifacts/codex_render_off_semantic_on_stable_20260425.json`
   - 稳定窗口报告：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_20_40_01.html`，`avgFps=253.767`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_20_40_13.html`，`avgFps=260.970`

3. 新硬结论
   - 在 `DXVK_WAR3_DISABLE=render`、isolated desktop、windowed、semantic.data 保留的口径下，稳定采样窗口已达到 `253~261 FPS`，接近/超过用户手测原版渲染约 `250 FPS` 的目标。
   - 本轮 summary：`semanticFrameRegistryPublishCalls=0`、`semanticFrameRegistryPublishUs=0`、`semanticModelGeosetResourceCalls=0`、`semanticModelHookCalls=486`、`semanticModelHookUs≈2.8ms`。这说明上层 semantic.data 在 render-off 口径下已经不是卡顿主因。
   - `render` 关闭时 control-plane ready 条件会天然超时：`gameStarted=false / inGameRenderReady=false`，但帧仍在推进并可产出 perf report。因此 render-off 性能测试必须使用 perf-only/stable-window 口径，不要把 ready timeout 误判为语义数据层卡死。
   - 短窗口报告会从约 `177 -> 220 -> 253+ FPS` 逐步抬升；性能结论必须取后段稳定窗口，不能取启动早期报告。

4. 当前 blocker
   - 上层数据层性能目标在“不开渲染层”口径下已达成。下一步应回到渲染消费层：找出 semantic shadow 实际绘制时的低 FPS 来源，尤其是 shadow map / semantic scene submit / native backend execute / legacy fallback 混合路径。
   - 不要再把 `VisibleRenderableRegistry::FinalizeVisibleRecord()` 放回 append 热路径；不要恢复 geoset hook 空转；不要用 control-plane ready gate 判断 render-off 性能。

#### 9.11.41 2026-04-25 full render 低压图：语义 skinned 默认化与 89 FPS 收口

1. 本轮改动
   - `War3Renderer::EndFrame()` 不再因为 semantic preview/runtime validation 模式而允许同一帧重复 publish/capture；frame-local registry publish 恢复 single-flight。
   - `D3D9DeviceEx::War3TryPopulateSemanticShadowScene()` 新增：
     - 同一 host frame 已成功提交相同 publishRevision 的 semantic scene 时直接跳过后续重复提交；
     - 仅在 cache 为空时用 `ModelRegistry` 种入 `runtimeModel/modelResource`；
     - 同轮把 runtime geoset 列表一起喂给 `ShadowModelResourceCache::noteRuntimeGeosetBinding(...)`，恢复 ready geoset；
     - `EnsureLatestFrameBuilt` 对“latest frame 已经 fresh”做短路；
     - `BootstrapCatchup` 在已有成功提交后降为单步追赶。
   - 默认打开 `kShadowSemanticCoreSceneBypassLegacyUnitCaptureEnabled=true`，不再只靠临时 env 才旁路 legacy unit fallback。
   - 新增 `War3SemanticScene/CaptureContract`、`EnsureLatestFrameBuilt`、`BootstrapCatchup`、`SubmitFrame` perf scopes，用于把 consumer 热点从 `Other/UntrackedActive` 里拆出来。

2. Fresh artifact
   - `AutoTest/artifacts/codex_low_pressure_manual_full_20260425.json`
   - `AutoTest/artifacts/codex_low_pressure_manual_full_after_singleflight_20260425.json`
   - `AutoTest/artifacts/codex_low_pressure_manual_full_after_runtime_geoset_seed_20260425.json`
   - `AutoTest/artifacts/codex_low_pressure_after_endframe_flush_skip_20260425.json`
   - `AutoTest/artifacts/codex_low_pressure_after_latest_shortcircuit_20260425.json`
   - `AutoTest/artifacts/codex_low_pressure_default_bypass_perfscope_20260425.json`
   - 关键报告：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_22_09_44.html`：`avgFps=63.771`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_22_15_55.html`：`avgFps=84.961`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_22_33_57.html`：`avgFps=89.036`

3. 新硬结论
   - 完整低压图 full render 口径下，semantic scene 重复提交 storm 已被压下：
     - 初始样本：`avgFps=63.771`，`semanticConsumerBuildCalls=4614`，`semanticContractCaptureCalls=4634`
     - single-flight 后：`avgFps=84.961`，`semanticConsumerBuildCalls=1533`，`semanticContractCaptureCalls=1551`
   - 现有数据层已经真正开始画 skinned semantic shadow，而不是纯观测：
     - runtime geoset seed 后：`shadowReadyGeosetCount=326`、`matrixPaletteCount=34`
     - `semanticCoreSkinnedResolved=37`
     - `semanticSceneSubmittedSkinned=37`
   - 默认打开 legacy unit capture bypass 并关掉正常 BeforeUi 宿主下的 endframe semantic flush 重复入口后，当前默认低压图已达到：
     - `avgFps=89.036`
     - `semanticSceneSubmittedSkinned=32`
     - `objectFallbackDrawCount=0`
   - 当前 89 FPS 不是“没走语义阴影”的假快，而是“语义 skinned 已提交、旧 object fallback 已清零”的真快。

4. 当前 blocker
   - 距离 `100+ FPS` 还差最后约 `11%` 左右，热点已收敛：
     - `War3SemanticScene/CaptureContract ≈ 1.18ms/frame`
     - `War3SemanticScene/SubmitFrame ≈ 0.29ms/frame`
     - `War3SemanticScene/BootstrapCatchup ≈ 0.23ms/frame`
     - `War3SemanticScene/EnsureLatestFrameBuilt ≈ 0.23ms/frame`
     - `Other/UntrackedActive ≈ 8.3ms/frame`
   - 下一轮精确入口：
     - 优先继续压 `CaptureContract`，特别是跨帧稳定 contract 的复用/去重；
     - 再看 `EnsureLatestFrameBuilt / BootstrapCatchup` 能否进一步在 steady-state 下旁路；
     - 不要把 legacy fallback、visible finalizer、geoset 空 hook 再放回热路径。

#### 9.11.42 2026-04-26 语义动态阴影性能门槛：低压图 100+ FPS 已越过

1. 本轮改动
   - 新增 `kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled` 与运行时 gate `DXVK_WAR3_SEMANTIC_SHADOW_DISABLE_LEGACY_CAPTURE`：
     - semantic preview + scene submission 生效时，`War3TryCaptureShadowCaster()` 直接跳过旧 draw-time `ShadowCapture`；
     - `War3RenderPipeline::OnFrameStart()` 同步把 `m_wantsShadowCapture` 的旧 capture 需求关掉，但保留 ShadowReceiver/ShadowMap/BeforeUi。
   - `AutoTest/war3_autotest_mcp.py` 的 semantic validation env 默认关闭 `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD`，避免 EndFrame 与 scene submit 双路 build。
   - AutoTest hot-shadow gate 修正：只有 `DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW=1` 时才要求 native backend executed draw；DXVK-only perf 验证只要求 semantic scene consumed。
   - `ShadowRuntimeContract` 的 direct CModel pose 读法小幅收缩：
     - `ShadowPoseStore/ShadowAttachmentRigidStore` 增加 reserve；
     - matrix hash 改为直接 hash War3 原生 48-byte palette，避免解码成 `Matrix4` 后再扫 64-byte 数组。

2. Fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 关键二分：
     - `AutoTest/artifacts/codex_low_pressure_disable_shadow_capture_probe_20260426.json`
       - `DXVK_WAR3_DISABLE=shadow.capture` 探针：`avgFps=125.148`、`semanticSceneSubmittedSkinned=79072`、`objectFallbackDrawCount=0`。
     - `AutoTest/artifacts/codex_low_pressure_semantic_disables_legacy_capture_20260426.json`
       - 仅 device 侧 gate：`avgFps=101.656`、`semanticSceneSubmittedSkinned=64320`、`objectFallbackDrawCount=0`。
     - `AutoTest/artifacts/codex_low_pressure_pipeline_gate_capture_off_20260426.json`
       - pipeline gate 后：`avgFps=115.397`、`CaptureContract=0.603ms`、`SubmitFrame=0.226ms`。
     - `AutoTest/artifacts/codex_low_pressure_no_endframe_build_probe_20260426.json`
       - 关闭 EndFrame build：`avgFps=123.280`、`semanticSceneSubmittedSkinned=77920`、`objectFallbackDrawCount=0`。
     - `AutoTest/artifacts/codex_low_pressure_semantic_optimized_gate_20260426.json`
       - 本轮最佳低压短窗：`avgFps=127.120`、`avgGpuTimeMs=1.727`、`CaptureContract=0.606ms`、`SubmitFrame=0.216ms`、`semanticSceneSubmittedSkinned=80320`、`objectFallbackDrawCount=0`。
   - 正式 preset：
     - `AutoTest/artifacts/codex_low_pressure_static_reuse_semantic_optimized_20260426.json`
       - `ok=true`、`hotShadowOk=true`、`avgFps=123.572`、`semanticSceneSubmittedSkinned=93728`、`objectFallbackDrawCount=0`。
     - `AutoTest/artifacts/codex_dynamic_shadow_pressure_semantic_optimized_20260426.json`
       - `ok=true`、`hotShadowOk=true`、`avgFps=40.447`、`avgGpuTimeMs=14.851`、`semanticSceneSubmittedSkinned=33504`、`objectFallbackDrawCount=0`。

3. 新硬结论
   - 低压图已经从上一轮 89 FPS 推到正式 gate 123 FPS / 短窗 127 FPS，明确超过旧 VB/IB snapshot 路线约 100 FPS 的最低门槛。
   - 当前可见阴影不是旧 capture/freeze 回退：`objectFallbackDrawCount=0` 且 `semanticSceneSubmittedSkinned > 0` 稳定成立。
   - `ShadowCapture` 旧模块是之前低压 full render 的主要额外税。全局 `shadow.capture` 关掉可到 125 FPS；本轮已把等价行为收进 semantic scene runtime gate，而不是依赖测试时手动禁模块。
   - EndFrame build 在当前 scene submit 单飞路径下不是必须项；关闭后 correctness 仍成立且低压 FPS 抬升。
   - `dynamic_shadow_pressure` 已通过 correctness gate，但性能已转为 GPU/ShadowMap 压力：`Shadow/Main GPU≈7.34ms`、`ShadowMap GPU≈6.94ms`，不再是上层 semantic.data 卡顿。

4. 当前 blocker
   - 低压目标：最低 `100+ FPS` 已达成，`130 FPS` 目标尚未稳定签收；当前最佳 127 FPS，剩余主要是 `Other/UntrackedActive≈6.6ms` 与 `CaptureContract≈0.6ms`。
   - 高压目标：`dynamic_shadow_pressure` correctness 通过，但 GPU bound 明显，下一轮要看 skinned caster draw 合并 / shadow map 分辨率自适应 / 级联剔除，而不是再追 owner/resource 逆向。
   - War3VK 剥离暂缓：语义路径已超过旧 snapshot 最低门槛，但尚未稳定达到 130 FPS 目标；先继续在 DXVK 基座完成性能收口，再抽 DLL ABI。

5. 下一轮精确入口
   - CPU 侧：继续压 `ShadowRuntimeContractCache::captureLiveState()` 的 direct pose copy/hash，目标 `CaptureContract < 0.5ms`。
   - GPU 侧：围绕 `War3ShadowReceiverPass::renderShadowMap()` 与 semantic skinned caster replay，做高压场景的 caster batch/cascade cull A/B；不能恢复旧 snapshot/freeze 主路径。
   - AutoTest：保留 `isolated desktop + windowed`；DXVK perf 测试继续禁止 `DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW=1`。

#### 9.11.43 2026-04-26 skinned caster cascade cull 收口：低压 130 FPS+ 达成，高压 ShadowMap 明显下降

1. 本轮改动
   - `War3ShadowReceiverPass::renderShadowMap()` 不再对 `vertexBlendEnabled` skinned draw 无条件放行到所有 cascade。
   - `kShadowCascadeCullDisableForUnits=false`，并为 skinned caster 加保守 padding：
     - `kShadowCascadeCullSkinnedRadiusScale=1.55`
     - `kShadowCascadeCullSkinnedExtraRadius=160`
     - `kShadowCascadeCullSkinnedExtraGuardNdc=0.24`
     - `kShadowCascadeCullSkinnedZExtraGuardNdc=0.32`
   - 目的：修远镜头/低 Z caster 底部裁剪风险，同时恢复安全 cascade culling，避免每个 skinned caster 固定重复打满所有 cascade。
   - AutoTest gate 增加 `near-latest` semantic scene consumption 判定：隔离桌面下 render-scene summary 允许 `sceneLag/frameSerialLag <= 2` 的小滞后，但仍要求 scene 有提交；防止把 tail summary lag 误判为 correctness 失败。

2. Fresh artifact
   - 构建：
     - `ninja -C build32 -j4` 通过（本轮最终构建无新工作）。
   - 统一工件：
     - `AutoTest/artifacts/codex_semantic_skinned_cascade_cull_20260426.json`
   - 低压图三轮 perf-only（全部 isolated desktop + windowed，native preview 关闭，legacy capture 禁止）：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_26_04_18_39.html`
       - `avgFps=139.723`、`avgGpuTimeMs=1.968`、`ShadowMap GPU=0.223ms`、`CaptureContract=0.603ms`、`SubmitFrame=0.224ms`
       - `semanticSceneSubmittedSkinned=124160`、`objectFallbackDrawCount=0`、`semanticCoreSkippedNoPose=0`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_26_05_25_16.html`
       - `avgFps=150.982`、`avgGpuTimeMs=1.785`、`ShadowMap GPU=0.167ms`、`CaptureContract=0.507ms`、`SubmitFrame=0.196ms`
       - `semanticSceneSubmittedSkinned=133984`、`objectFallbackDrawCount=0`、`semanticCoreSkippedNoPose=0`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_26_05_26_33.html`
       - `avgFps=149.187`、`avgGpuTimeMs=1.830`、`ShadowMap GPU=0.202ms`、`CaptureContract=0.508ms`、`SubmitFrame=0.212ms`
       - `semanticSceneSubmittedSkinned=132384`、`objectFallbackDrawCount=0`、`semanticCoreSkippedNoPose=0`
   - dynamic pressure perf-only：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_26_04_19_39.html`
     - `avgFps=114.928`、`avgGpuTimeMs=6.382`、`ShadowMap GPU=2.878ms`、`Shadow/Main GPU=3.136ms`
     - `semanticSceneSubmittedSkinned=102272`、`objectFallbackDrawCount=0`、`semanticCoreSkippedNoPose=0`

3. 新硬结论
   - 低压 130 FPS 目标已达成：
     - 三轮 `avgFps=139.723 / 150.982 / 149.187`
     - `median=149.187`
     - `worst=139.723`
   - 这不是回退路径假快：三轮均 `objectFallbackDrawCount=0` 且 `semanticSceneSubmittedSkinned > 0`。
   - dynamic pressure 的 correctness 仍保持 semantic-only，并且 ShadowMap GPU 压力从上一轮正式 gate 的约 `6.94ms` 降到 `2.878ms`；瓶颈仍偏 GPU/ShadowMap，但已经不是“所有 skinned caster 无条件打满 cascade”的灾难形态。
   - 低 Z/远镜头缺底问题本轮按“扩大 skinned bounds 后再 cull”的方式处理；低压截图 `war3_20260426_052635.png` 显示 skinned 单位/飞行单位阴影仍可见，后续可用用户固定视角继续人工视觉验收。

4. 当前 blocker / Caveat
   - MCP-hosted `run_named_scenario(low_pressure_static_reuse)` 本轮有一次工具层 1800s timeout，留下一个 responsive War3 进程；已手动清理。
   - 本次 timeout 没有生成 perf report，且本地直调 `run_quick_autotest()` 连续三轮成功；因此先判定为 MCP 服务/strict gate 热加载问题，不作为 runtime correctness 失败。
   - `AutoTest/war3_autotest_mcp.py` 已落 near-latest gate 修正；若 MCP 服务常驻未重载，下一线程需要重启 MCP 或用本地直调验证新版 gate。

5. 下一轮精确入口
   - DXVK 主线已满足迁移前性能门槛：可以开始 `E:\Mycode\Source\Repos\War3VK` ASI late-inject 骨架。
   - War3VK 第一轮只做 ABI/Bootstrap/Load marker/生命周期统计，禁止提前搬 DXVK 私有 renderer 大对象。
   - DXVK 侧后续优化重点转为：
     - high pressure 的 skinned caster draw 合并 / cascade 复用；
     - `CaptureContract≈0.51ms` 的 direct pose cache 尾巴；
     - `Other/UntrackedActive` 拆 scope，但不恢复 legacy snapshot/freeze。

#### 9.11.44 2026-04-26 War3VK/ASI late-load 骨架已创建并验证自动加载

1. 本轮改动
   - 创建独立工程：`E:\Mycode\Source\Repos\War3VK`
   - 第一版只包含：
     - `DllMain` late-load bootstrap；
     - worker thread 等待 `Game.dll`；
     - load marker 文件输出；
     - `BindDevice / PrepareFrame / ExecutePreparedFrame / Reset / GetStats` ABI；
     - `War3VK_*` 诊断导出；
     - `War3VKLoadSmoke.exe` 本地 `LoadLibraryA` 验证工具。
   - `DllMain` 约束：
     - 只 `DisableThreadLibraryCalls`、初始化路径、写 marker、创建 worker；
     - 不在 loader lock 内做 hook、D3D 初始化或渲染初始化。

2. Fresh artifact
   - `AutoTest/artifacts/codex_war3vk_asi_bootstrap_20260426.json`
   - MSBuild：
     - `E:\Mycode\Source\Repos\War3VK\War3VK.vcxproj`
     - `Release|Win32` 使用 VS 2026 MSBuild 成功，`0 warning / 0 error`
     - 产物：`E:\Mycode\Source\Repos\War3VK\bin\Win32\Release\War3VK.dll`
   - 依赖：
     - 改为静态 CRT `/MT` 后，`dumpbin /dependents` 只剩 `KERNEL32.dll`
   - 导出：
     - 未修饰 ABI：`BindDevice / PrepareFrame / ExecutePreparedFrame / Reset / GetStats`
     - 诊断 ABI：`War3VK_GetAbiVersion / War3VK_GetStats / ...`
   - 本地 load smoke：
     - `War3VKLoadSmoke.exe War3VK.dll`
     - `LoadLibraryA ok`
     - `abiVersion=1`
     - `GetStats` 成功
   - War3 ASI smoke：
     - `War3VK.dll` 复制为 `E:\Work\War3\War3VK.asi`
     - 删除临时 `SchattenBoost.dll` 后，仅保留 `.asi` 启动魔兽；
     - isolated desktop + windowed 低压图 smoke 中生成：
       - `E:\Work\War3\War3VK\Log\War3VK_bootstrap.log`
       - `DllMain attached`
       - `Bootstrap worker started`
       - `Game.dll detected base=0x69370000 after 0 ms`
       - `Bootstrap ready; waiting for host BindDevice/PrepareFrame calls`
     - 结束后无 War3 残留进程。

3. 新硬结论
   - ASI 自动加载链可行：`War3VK.asi` 放在当前 War3 根目录即可被加载，不需要 `SchattenBoost.dll` 固定名。
   - War3VK 骨架本体可被普通 32-bit 进程和 War3 进程加载，且不依赖 VC runtime 分发。
   - 现在迁移边界明确：下一步不是搬 DXVK renderer，而是先让 DXVK host 通过 ABI 调用 `BindDevice/GetStats`，再逐步把 renderer submission 数据结构做成可共享 contract。

4. 当前 blocker
   - War3VK 目前只是 bootstrap/ABI proof，未绑定 D3D9 device，也未执行渲染。
   - 下一阶段需要在 DXVK host 侧新增可选动态加载 `War3VK.asi/War3VK.dll` 的桥：
     - `LoadLibrary/GetProcAddress`
     - `BindDevice` 于 device ready 后调用；
     - 每帧可选 `PrepareFrame/ExecutePreparedFrame/GetStats`
   - 保持默认关闭，不影响当前 DXVK semantic shadow 性能验证。

5. 下一轮精确入口
   - `E:\Mycode\Source\Repos\War3VK`：
     - 增加 device lifecycle stub 细化；
     - 增加 ABI stats JSON/log 输出；
     - 准备与 DXVK host 共享的最小 frame packet 描述。
   - DXVK repo：
     - 新增可选 War3VK bridge 模块，动态加载 ABI，默认关闭；
     - 不移动 renderer 大对象，先验证 late-inject ABI 生命周期。

#### 9.11.45 2026-04-30 semantic-only units 基线恢复，静态 hydrate 主清单路径证伪

1. 本轮改动 / 回退
   - 复核当前可运行基线：正式路径仍为 `visible manifest -> direct CModel pose/palette read -> ShadowRuntimeContract -> ShadowRendererCore -> DXVK shadow backend`，并保持 `kShadowSemanticCoreSceneUnitsOnly=true`。
   - 试验性打开 `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate` 并允许 units-only 模式帧末 hydrate Building/Destructible，只作为“不提交静态阴影”的诊断。
   - 该试验被证伪后已立即撤回：静态 hydrate 在当前主清单上会干扰 skinned semantic scene 的 resource/frame 选择，不能混入正式 units-only 热路径。

2. Fresh artifact
   - 构建：
     - `ninja -C build32 -j4` 通过。
   - 恢复后低压图隔离桌面窗口化复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_02_42_45.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_024246.png`
     - `avgFps=127.891`
     - `avgGpuTimeMs=1.951`
     - `avgMainThreadCpuMs=4.906`
     - `objectFallbackDrawCount=0`
     - `fallbackDrawCount=0`
     - `semanticSceneSubmittedSkinned=48504`
     - `semanticCoreSubmittedDrawCount=40`
     - `semanticCoreSkippedNoPose=0`
     - `semanticCoreSkippedNoRuntimeGroupPalette=0`
     - `shadowReadyGeosetCount=326`
   - 本轮静态 hydrate 失败工件：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_02_40_06.html`
     - `E:\Work\War3\WarVK\Crash\war3_crash_2026_04_30_02_40_06_678_pid11528_tid27112.dmp`
     - 失败读数：`semanticSceneSubmittedSkinned=0`、`semanticSceneSubmitted=0`、`shadowReadyGeosetCount=25`、`semanticCoreSubmittedDrawCount=12`。

3. 新硬结论
   - 当前 semantic-only 动态单位阴影路径是可运行的：低压图恢复后仍有 skinned semantic submission，且 `objectFallbackDrawCount=0`。
   - `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate` 不能直接用于当前 units-only 主清单，即使“不提交静态阴影”也会破坏 scene submission/resource readiness，并可触发崩溃。
   - 静态 doodad/destructible/building 阴影缺失的下一步不能再通过“在主 visible snapshot 上就地 mutate/hydrate”解决；必须改成独立 shadow-side 静态候选缓存或离线影子统计，确认 resource/geoset/visibility 后再单独提交。

4. 当前 blocker
   - 动态单位 skinned path 已恢复到 no-fallback 可用态，但本轮 10s 短窗为 `127.891 FPS`，略低于之前两轮 `132 FPS`；仍高于 100 FPS 门槛，后续需继续复测/压 `CaptureContract≈0.79ms`。
   - 静态对象路径仍未闭环：Building/Destructible 在 ready status 中可见，但不允许通过当前 static hydrate 写回主清单。

5. 下一轮精确入口
   - 实现只读的 static-caster shadow candidate observer：
     - 不修改 `VisibleRenderableRegistry::Snapshot`；
     - 不调用热路径全量 `ResolveGeosetMetadata`；
     - 只统计 Building/Destructible/Doodad 的 `meshData` 形态、resource cache 命中、runtimeGeoset/Data 命中和 reject reason；
     - summary/control-plane 输出静态候选计数。
   - 只有 observer 证明静态候选 resource/geoset 稳定后，再做单独的 static shadow store 和安全提交白名单；禁止再次打开主清单 static hydrate 作为 shortcuts。

#### 9.11.46 2026-04-30 stale caster frame 修复与 semantic-only 低压 130+ 恢复

1. 本轮改动
   - 针对用户视觉确认的“一个 caster 的部分阴影像被套到所有 caster 上”问题，定位到 semantic scene completed frame 长期复用：上一轮报告中 `semanticCoreFrameLag=1297`。
   - 在 `War3TryPopulateSemanticShadowScene` 中恢复 steady build drain，但限制为低频周期推进，避免回到 build storm。
   - 在 DXVK scene submit 阶段为 skinned packet 直接从当前 `CModel +0x5C/+0x60` 重建 runtime group palette，并用 `ShadowPacketResource` 携带 `MatrixGroupSizes/MatrixIndices`，让动画 palette 用 live CModel 保鲜。
   - 禁止 resource-matched pose rescue 在 scene submission runtime 中跨实例借 pose，避免同模型多 caster 共用一份旧姿态。
   - 将 contract capture 也改为首帧闭环后周期刷新；packet/topology 可轻微滞后，skinned animation 由 submit-time live palette 刷新。
   - 修正 summary 口径：scene submission 复用 last completed frame 时，`semanticCoreSubmittedDrawCount` 不再误报为 0，而是回填有效 reusable packet count。

2. Fresh artifact
   - 构建：
     - `ninja -C build32 -j4` 通过。
   - 低压 quick 2K 窗口验证：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_05_50.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_040551.png`
     - `avgFps=135.540`
     - `avgGpuTimeMs=1.902`
     - `avgMainThreadCpuMs=4.338`
     - `objectFallbackDrawCount=0`
     - `fallbackDrawCount=0`
     - `semanticSceneSubmittedSkinned=59936`
     - `semanticCoreSkippedNoPose=0`
     - `semanticCoreSkippedNoRuntimeGroupPalette=0`
     - `semanticCoreFrameLag=8`
   - 低压 quick summary fix 复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_08_51.html`
     - `avgFps=135.009`
     - `semanticCoreSubmittedDrawCount=40`
     - `semanticSceneSubmittedSkinned=59712`
   - `low_pressure_static_reuse` named scenario：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_12_41.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_041218.png`
     - `avgFps=155.758`（1902x963 windowed）
     - `semanticSceneSubmittedSkinned=112576`
     - `semanticCoreSubmittedDrawCount=40`
     - `objectFallbackDrawCount=0`
   - `dynamic_shadow_pressure` named scenario：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_14_08.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_041409.png`
     - `avgFps=40.689`
     - `avgGpuTimeMs=12.227`
     - `ShadowMap GPU≈4.257ms`
     - `semanticSceneSubmittedSkinned=33728`
     - `semanticCoreSubmittedDrawCount=36`
     - `objectFallbackDrawCount=0`
   - `model_runtime_probe` named scenario：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_17_06.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_041643.png`
     - `avgFps=52.149`
     - `shadowRuntimeModelCount=185`
     - `shadowReadyGeosetCount=1394`
     - `semanticSceneSubmittedSkinned=31520`
     - `semanticCoreSubmittedDrawCount=36`
     - `objectFallbackDrawCount=0`

3. 新硬结论
   - 错影主因不是 resource-matched pose rescue；该类 rescue counters 在复测中为 0。真正危险点是 completed semantic frame 停在旧 revision，被后续 caster 复用。
   - periodic build + periodic capture + submit-time live CModel palette 可以同时满足：
     - 不回退 VB/IB snapshot/freeze；
     - `objectFallbackDrawCount=0`；
     - 低压图恢复到 130 FPS+；
     - 截图中 skinned unit shadow 仍可见且不再出现灰色 veil / scaffold 静态 hydrate 污染。
   - `semanticCoreFrameLag` 现在代表 packet/topology frame 可轻微滞后；skinned animation freshness 由 submit-time live palette 保证。后续如果要把该指标变成正式 gate，需要新增“live palette refreshed”专用 freshness counter，而不是继续要求 frameLag<=1。
   - `dynamic_shadow_pressure` 已不是上层 semantic.data 卡顿；当前高压瓶颈是 GPU/ShadowMap 与大量 caster 场景。

4. 当前 blocker
   - 静态 doodad/destructible/building 阴影仍未闭环；主清单 static hydrate 已证伪，不能重开。
   - 高压图 `dynamic_shadow_pressure` 仍只有约 40 FPS，主要受 `ShadowMap GPU≈4.257ms` 与总体 GPU≈12.227ms 影响，需要继续做 cascade/caster 合并或自适应 shadow map。
   - `semanticCoreFrameLag` 在 periodic 模式下会保持 8~20 左右，当前视觉/提交由 live palette 弥补；后续 control-plane 应拆分 topology freshness 与 pose/palette freshness。

5. 下一轮精确入口
   - 性能：`d3d9_war3_shadow.cpp` 的 ShadowMap/cascade culling、skinned caster draw 合并、shadow map 更新复用。
   - 正确性：为 submit-time live palette 增加 summary counters，例如 `semanticSceneLivePaletteRefreshCount / MissCount`，让 gate 能明确知道动画 freshness 来自 CModel 直读。
   - 静态对象：只做只读 observer 和独立 candidate store，继续禁止 `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate` 写主清单。

#### 9.11.47 2026-04-30 semantic submit hot-path 续航与 live palette 解码优化

1. 本轮改动
   - 接续自动推进中断点后，先清理/确认无残留 `War3/Warcraft` 进程，继续保持所有 runtime 测试 `isolated desktop + windowed`。
   - 针对 `Hook_PublishVisible/FillUnitIdentity` 曾经出现的热路径 `VirtualQuery` 风暴，保留当前 `unitPtr -> rawcode/flags/handle/kind` TLS hot cache：cache miss 才做一次 readable check，cache hit 直接合并 identity。
   - `War3TryBuildLiveRuntimeGroupPalette` 从“每个 skinned packet 全量解码 `CModel +0x60` final pose array 到 `std::vector<Matrix4>`”改为“按 geoset matrix contract 按需解码，并在单次调用内用 TLS generation cache 避免重复解码同一个 matrix index”。
   - 试验过进一步裁剪 `matrixGroupSizes` 输出到 `maxVertexGroupSlot + 1`，但低压复测从约 94 FPS 回落到约 87 FPS，判定为负收益，已撤回该子改动。
   - `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD` 默认保持 240，原因是 submit-time live CModel palette 已负责动画 freshness，contract capture 只需要周期刷新 topology/manifest。

2. Fresh artifact
   - 构建：
     - `ninja -C build32 -j4` 通过。
   - 低压图 named scenario / live palette 按需解码首轮：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_20_02_16.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_200153.png`
     - `avgFps=94.386`
     - `avgGpuTimeMs=1.280`
     - `avgMainThreadCpuMs=6.420`
     - `avgUntrackedActiveCpuMs=9.754`
     - `semanticSceneSubmittedSkinned=81583`
     - `semanticSceneLivePaletteRefreshHitCount=81583`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticSceneSkinnedDynamicIndexSliceCount=81583`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 低压图当前最终复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_20_09_11.html`
     - `avgFps=87.795`
     - `avgGpuTimeMs=1.533`
     - `avgMainThreadCpuMs=6.555`
     - `avgUntrackedActiveCpuMs=10.493`
     - `semanticSceneSubmittedSkinned=68230`
     - `semanticSceneLivePaletteRefreshHitCount=68230`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticCoreSubmittedDrawCount=50`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`

3. 新硬结论
   - 当前可见阴影仍是 semantic-only skinned 路线：`objectFallbackDrawCount=0`、`fallbackDrawCount=0`、`semanticSceneSubmittedSkinned>0`、`semanticSceneSkinnedFullIndexFallbackCount=0`。
   - `FillUnitIdentity` 的 `VirtualQuery` 风暴已经不再是 top cost；上一轮 hot cache 后该 scope 从热榜消失。
   - live palette 全命中：`semanticSceneLivePaletteRefreshHitCount == AttemptCount` 且 Miss 为 0，说明 submit-time `CModel +0x5C/+0x60` 直读链稳定。
   - 当前 FPS 波动较大，低压 named scenario 在同一 evening run 约 87~94 FPS；它已接近 no-semantic-scene fallback 基线（约 89 FPS），但未恢复凌晨 4 点的 135/155 FPS 档位。剩余 blocker 不再是“上层数据拿不到/数据层卡死”，而是 `SubmitFrame`、`ShadowMap` 与 `Other/UntrackedActive`。

4. 当前 blocker
   - `War3SemanticScene/SubmitFrame` 仍有约 0.47ms/frame 级别尾巴；需要继续拆 `War3TryAppendSemanticShadowPacket` 内 palette lookup、geometry append、bounds、instance push 的分段计时。
   - `Other/UntrackedActive` 仍有约 9~10ms active gap；需要确认是 War3 原生 main-loop/地图脚本/healthbar，还是仍有未加 scope 的本项目热路径。
   - 静态 doodad/destructible/building 阴影仍未进入 semantic submit；`frame summary` 已能看到 destructible 9 条有 mesh/resource/geoset，但主路径仍 `kShadowSemanticCoreSceneUnitsOnly=true`。主清单 static hydrate 已证伪，不能重开。

5. 下一轮精确入口
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：新增临时可控 perf scope 或结构化 counters，拆 `LivePaletteBuild / PaletteIndex / GeometryLookupCreate / Bounds / PushInstance`。
   - `War3TryBuildLiveRuntimeGroupPalette`：如果继续优化，优先做 resource-level remap plan cache，而不是再裁 palette group count。
   - `d3d9_war3_shadow.cpp`：继续检查 ShadowMap/cascade GPU 与 caster culling。
   - 静态对象只做只读 observer/独立 store，不修改 main visible manifest，不打开 `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate`。

#### 22.14.14 2026-04-30 semantic submit cap/owned dynamic index continuation

1. 本轮改动
   - 接续上午自动推进挂住后的状态，先确认无残留 `War3/Warcraft III` 进程；所有 runtime 测试继续使用 isolated desktop + windowed。
   - 保留并验证 owned dynamic index slice 路线：semantic packet 不再长期借用 War3 hot index pointer，避免 stale completed frame 把某个 caster 的 index slice 套到其他 caster。
   - `ShadowRendererCore` 新增 `submitFrameLimited(frame, backend, maxSubmittedDraws)`，semantic submit cap 改为 submit loop 内限额，停止创建临时 `cappedFrame.draws` 拷贝。
   - `DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP` 新增 runtime override，默认仍走 `kShadowSemanticCoreSceneSubmitDrawCap=64`，后续可以无重编译测 32/48/96。

2. Fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 低压图 cap=128 基线：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_21_43_12.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_214249.png`
     - `avgFps=104.344`
     - `semanticSceneSubmittedSkinned=97980`
     - `semanticSceneLivePaletteRefreshHitCount=97980`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticCoreSubmittedDrawCount=49`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 低压图 cap=64 + limited submit：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_05_19.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_220456.png`
     - `avgFps=108.835`
     - `semanticSceneSubmittedSkinned=105205`
     - `semanticSceneLivePaletteRefreshHitCount=105205`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticCoreSubmittedDrawCount=49`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 高压图 cap=64 + limited submit：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_06_36.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_220637.png`
     - `avgFps=29.134`
     - `avgGpuTimeMs=8.826`
     - `avgTrackedActiveCpuMs=2.083`
     - `avgUntrackedActiveCpuMs=32.241`
     - `War3SemanticScene/SubmitFrame≈1.017ms`
     - `semanticSceneSubmittedSkinned=22896`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 高压图 runtime cap=32 试验：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_13_29.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260430_221331.png`
     - `avgFps=31.028`
     - `avgTrackedActiveCpuMs=1.258`
     - `avgUntrackedActiveCpuMs=30.970`
     - `semanticSceneSubmittedSkinned=11744`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`

3. 新硬结论
   - 当前低压图可见阴影仍是 semantic-only skinned path：`semanticSceneSubmittedSkinned>0`、live palette miss 为 0、`semanticSceneSkinnedFullIndexFallbackCount=0`、`objectFallbackDrawCount=0`。
   - cap=64 对低压图不削减 draw（当前 core submitted draw 约 49），并且相比 cap=128 有小幅正收益。
   - 高压图已从上一轮 fast-score 后约 26.86 FPS 推到 cap=64 的 29.13 FPS；cap=32 可到 31.03 FPS，但属于 caster 数量/性能 tradeoff，暂不把默认值降到 32。
   - no-shadow/no-semantic perf-only 试验生成了 `273 FPS` 报告，但 runtime status 显示 `inGameRenderReady=false / visibleCount=0`，不是有效进图 baseline，禁止作为“原版低压对照”引用。

4. 当前 blocker
   - 低压图仍未恢复凌晨 4 点 `155 FPS` 档位；对比 `04:12` artifact，主要差异是 `Other/UntrackedActive` 从 `5.861ms` 升到 `8.580ms`，不是 semantic submit 本身。
   - 高压图最大 blocker 是原生/未归因 main-loop active gap（约 31~32ms）与 GPU 总时间（约 8~9ms）；当前 semantic tracked CPU 已压到约 1.2~2.1ms。
   - 静态 doodad/destructible/building 仍未进入正式 semantic shadow；继续禁止 static hydrate 写 main visible manifest。

5. 下一轮精确入口
   - `AutoTest/war3_autotest_mcp.py`：补一个真正可比较的 perf-only in-map baseline runner，不能依赖 shadow hot gate。
   - `war3_hook_lifecycle.cpp` / `war3_internal_test_config.h`：临时启用或新增低侵入 MainLoop active gap 采样，定位 `Other/UntrackedActive` 为什么从 5.8ms 升到 8.5ms。
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：继续压 `SubmitFrame` self 部分，优先考虑 palette cache keyed lookup / packet append micro-scope。
   - 静态对象：只读 observer + 独立 candidate store，不重开 static hydrate 主清单。

6. 22.21 续补
   - `AutoTest/war3_autotest_mcp.py` 增加 `perf-only-ready-timeout` 出口：当调用方明确 `require_control_plane_ready=False` 且 `record_after_game_started=False` 时，ready timeout 仍会收集并返回新 perf report，避免无人值守基线矩阵直接挂死。
   - 语法验证：`python -m py_compile AutoTest\war3_autotest_mcp.py` 通过。
   - Fresh artifact：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_21_14.html`，`avgFps=276.395`，但 `inGameRenderReady=false / visibleCount=0`，仍只能作为非进图 perf-only smoke，不可作为低压实战 baseline。
   - 下一步仍是给 shadow-off 路径补独立 in-map/world-ready marker，而不是依赖 shadow hot gate。

#### 02.30 2026-05-01 dynamic-pose root cause correction

1. 本轮主线
   - 按用户视觉反馈优先停止 FPS 方向，专查“阴影锁在模型初始化姿态”的动态姿态问题。
   - 新增 live CModel palette motion counters，直接验证 submit-time `CModel +0x5C/+0x60` raw pose 是否随动画变化。
   - 保持所有 runtime 测试为 isolated desktop + windowed，并在测试后强制清理 War3 进程。

2. Fresh artifact
   - live CModel motion 诊断（默认 live refresh 打开时）：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_01_52_50.html`
     - `semanticSceneLivePaletteRefreshHitCount=66502`
     - `semanticSceneLivePaletteMotionSampleCount=66502`
     - `semanticSceneLivePaletteMotionRawChangedCount=0`
     - `semanticSceneLivePaletteMotionRawStableCount=66481`
   - `+0xA0/runtime/-0xA0` alias 试探：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_01_58_27.html`
     - `semanticSceneLivePaletteMotionRawChangedCount=0`
     - `semanticSceneLivePaletteMotionRawStableCount=69557`
   - 禁用 submit-time live refresh 后低压复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_02_17_56.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260501_021733.png`
     - `semanticSceneLivePaletteRefreshAttemptCount=0`
     - `semanticSceneSubmittedSkinned=73966`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 动态姿态序列截图：
     - `AutoTest/artifacts/screenshots/dynamic_pose_check_20260501_022220/pose_01.png`
     - `AutoTest/artifacts/screenshots/dynamic_pose_check_20260501_022220/pose_04.png`
     - 同一场景间隔约 3 秒，单位动作与 shadow silhouette 同步变化。
   - 最终 opt-in frame-local 桥后复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_02_27_04.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260501_022642.png`
     - `avgFps=86.725`
     - `semanticSceneSubmittedSkinned=73350`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=73350`
     - `semanticSceneLivePaletteRefreshAttemptCount=0`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 动作压力图复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_02_33_41.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260501_023320.png`
     - `avgFps=32.651`
     - `semanticSceneSubmittedSkinned=19408`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneLivePaletteRefreshAttemptCount=0`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`

3. 新硬结论
   - `CModel +0x5C/+0x60` submit-time live palette 对当前 visible skinned runtime 是静态/非动画源；同一 runtime raw hash 在 6~7 万次样本中没有变化。
   - `runtime +0xA0` alias 也没有提供动画 raw pose；不能把 `+0xA0` 当成最终 palette producer。
   - 04-30 的 submit-time live CModel palette refresh 会覆盖 `ShadowRendererCore` 已构建好的 packet runtimeGroupPalette，从而把阴影锁回模型初始化/静态姿态。
   - 正式动态姿态来源应为 `visible manifest -> ShadowRendererCore packet runtimeGroupPalette -> D3D9DeviceEx::War3TryAppendSemanticShadowPacket`，不是 submit-time 直读 CModel。
   - frame-local dynamic mesh rescue 在当前低压单位主路没有命中（`semanticSceneSubmittedFrameLocal=0`），因此不能作为本轮动态姿态修复方案；已改回显式诊断 opt-in，避免 per-frame upload 或错影复发。

4. 本轮代码策略
   - `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` 新增为显式 opt-in，默认关闭，避免静态 CModel palette 再覆盖 packet palette。
   - `kShadowSemanticCoreAllowFrameLocalDynamicGeometry`、`kShadowSemanticCorePreferFrameLocalDynamicMeshForSkinned`、`kShadowSemanticCoreTreatFrameLocalDynamicMeshAsPreSkinned` 均恢复为 false，仅保留诊断。
   - 保持 no-fallback gate：`objectFallbackDrawCount=0`，没有恢复 VB/IB snapshot/freeze object shadow 主路径。

5. 当前 blocker
   - 动态 pose 锁初始化的问题已定位并止血，但低压 FPS 回到约 86~92，而不是 04-26 的 130+ 档；后续性能优化应以 packet palette 正式路径为基础，不再重新打开 submit-time CModel refresh。
   - 静态 doodad/destructible/building 仍未进入正式 semantic shadow；继续禁止 `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate` 写主 visible manifest。
   - 还需要针对“单位脚下阴影是否完全对齐当前 caster”和 ShadowMap bounds/cascade 做视觉验收，不要只看 FPS。
   - 动作压力图 correctness 维持 no-fallback skinned 提交，但 FPS 仍低；这是下一阶段性能问题，不应再用静态 CModel refresh “优化”。

6. 下一轮精确入口
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：继续围绕 packet runtimeGroupPalette 做 palette cache / paletteIndex / geometry lookup 优化；禁止默认启用 `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH`。
   - `ShadowRendererCore::tryResolveSkinnedRenderable`：如需继续动态姿态证据，增加 packet palette motion counters，而不是再用 CModel raw hash 判断动态。
   - `d3d9_war3_shadow.cpp`：检查当前 packet palette 路线下 caster bounds、cascade culling、低 Z 缺底和单位脚下 shadow alignment。

#### 04.43 2026-05-01 dynamic-pose producer correction v2

1. 用户新反馈
   - 性能已可接受，但阴影仍像模型初始化姿态；位置/阴影约 3 秒刷新一次，中间会短暂消失，太阳方向仍更新。
   - 这说明 receiver / shadow map / world-position 路径仍活着，真正问题在 skinned palette producer 与 semantic packet freshness。

2. 新硬结论
   - IDA/objdump 证据确认 `0x6F12FF50` 是 runtime matrix flush/reset：它会把 `CModel+0x60` 填成 shared global matrix，不能作为动画姿态 producer。
   - `0x6F12FDC0` 是 authoritative animated matrix range-copy：应从其 `context + source matrix base` 读取正在拷贝的源矩阵段。
   - `capturePoseOnlyLiveState()` 不能 bump manifest `publishRevision`，否则 pose 高频变化会让 `ShadowRendererCore` 一直追 revision，造成 stale/empty submit 空窗。

3. 本轮代码修正
   - `Hook_RuntimeMatrixFlush` 只保留 `RecordRuntimeMatrixPublisher(runtimeModel, 2)` 诊断，不再调用 `RecordRuntimeMatrixPalette`。
   - `Hook_RuntimeMatrixRangeCopy` 优先通过 `RecordRuntimeMatrixPaletteFromRangeCopy(runtimeModel, a2, a3)` 发布 source-range palette，失败才回退 `CModel+0x60`。
   - `War3SemanticLivePaletteRefreshRuntime` 默认重新开启，但提交侧优先从 `PoseRegistry` 的 source-range palette 刷新，不再盲读静态 `CModel+0x60`。
   - `ShadowRuntimeContractCache::capturePoseOnlyLiveState` 只替换 `m_poses/m_stats`，不改 manifest revision、不请求 rebuild。

4. Fresh artifact
   - 全量 capture dirty 反例：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_03_58_58.html`
     - `avgFps=9.615`
     - `CaptureContract avg=77.655ms`
     - `ManifestCopy avg=76.855ms`
   - pose-only capture 初版：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_04_10_51.html`
     - `avgFps=84.259`
     - `semanticSceneSubmittedPaletteMotionChangedCount=1088`
     - `objectFallbackDrawCount=0`
   - flush 禁止 + source-range publisher + live PoseRegistry refresh：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_04_40_56.html`
     - `avgFps=83.046`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreSubmittedDrawCount=47`
     - `semanticSceneSubmittedSkinned=58904`
     - `semanticSceneLivePaletteRefreshHitCount=58904`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticSceneSubmittedPaletteMotionChangedCount=58887`
     - `objectFallbackDrawCount=0`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260501_044032.png`

5. 当前 blocker
   - 正确性主 blocker 已从“无动态 palette”转为“需要人工/多帧视觉确认 source-range palette 下 silhouette 是否完全贴合动作”。
   - 性能回落到约 83 FPS，主要因为 submit-time live PoseRegistry refresh 每次提交都会刷新 skinned palette；这是 correctness 优先的可接受中间态，后续再做 palette cache keyed lookup。

6. 下一轮精确入口
   - `D3D9DeviceEx::War3TryBuildLiveRuntimeGroupPalette`：给 PoseRegistry live refresh 增加 keyed cache，key=`runtimeModelPtr + PoseRecord.matrixHash + resource.contentHash/matrix signature`。
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：继续拆 `SubmitFrame` self，优先压 `PaletteIndex` 与 live refresh remap 成本。
   - 视觉验收：用多帧截图或短序列确认单位 stand/attack/移动动作下 shadow silhouette 不再保持 bind pose；同时继续检查低 Z / bounds / cascade。

## 2026-05-01 06:42 +08:00 - pose-only frame freshness 修正

1. 本轮目标
   - 用户反馈当前性能可接受，但阴影仍保持模型初始姿态；单位移动时阴影位置约每 3 秒更新一次，姿态不更新，并伴随短暂消失。
   - 优先修正动态姿态链路，不再只追 FPS。

2. 新增硬结论
   - `0x6F12FDC0` source-range matrix publisher 仍在高速发布动画矩阵：低压图 `runtimeMatrixRangeCopyPalettePublishHitCount` 可从数百增长到数万，`runtimeMatrixRangeCopyPaletteFallbackCModelCount=0`。
   - 旧症状的当前主因不是“拿不到动画矩阵”，而是 semantic core 只按 `publishRevision` 判断 fresh；pose-only contract 为了避免拓扑 rebuild 保持 revision 不变，只推进 `frameSerial`，导致 `semanticCoreFrameSerial` 卡在首个完成帧。
   - 修复前工件 `AutoTest/artifacts/codex_pose_only_rebuild_20260501/result.json`：`semanticCoreFrameSerial=145` 固定，`semanticCoreManifestFrameSerial` 持续推进到 1287，`semanticSceneLivePaletteRefreshAttemptCount=0`，`objectFallbackDrawCount=0`。

3. 代码修正
   - `D3D9DeviceEx::War3TryPopulateSemanticShadowScene`：fresh/complete/catchup 逻辑增加 `frameSerial` 判断，不再只看 `publishRevision`。
   - `War3ShouldPreferSemanticSceneFrame` 与 `ShouldPreferRenderableSubmissionFrame`：同 revision、同 draw/skin 分数时优先选择更新的 `frameSerial`。
   - steady supplemented build 从 `1 pass / 4 frames` 改为 `2 passes / frame`，让 pose-only frame 能持续推进，避免 3 秒级全量 contract 周期。
   - `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` 保持 diagnostic 默认关闭；不得默认恢复 submit-time CModel refresh。

4. fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 计数复测：`AutoTest/artifacts/codex_pose_frame_freshness_20260501/result.json`
     - `semanticCoreFrameSerial` 从 169 持续推进到 959，`semanticCoreFrameLag` 通常 1-4。
     - `semanticSceneSubmittedPaletteMotionChangedCount=34` 的非零提交帧持续出现。
     - `objectFallbackDrawCount=0`。
   - 截图复测：`AutoTest/artifacts/codex_pose_visual_probe_20260501/frame_1.png`、`frame_2.png`、`frame_3.png`
     - 截图链路可用，无全屏/前台切换；画面未复现整屏黑滤镜和 scaffold 阴影。

5. 当前 blocker
   - summary 中仍会出现 build 中间态 `semanticSceneLastSubmittedDrawCount=0` 的采样；需要区分是否只是控制面采样撞到非提交帧，还是实际 shadow map 空窗。
   - 视觉层仍需用户或视频序列确认 silhouette 是否完全脱离 bind pose；本轮 counter 已证明 core packet 不再固定首帧。

6. 下一轮精确入口
   - `D3D9DeviceEx::War3TryPopulateSemanticShadowScene`：增加 `last reusable frame forced reuse` 的 explicit counter，确认中间态是否真的走了 fallback-to-last-frame。
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：增加 per-runtime packet `runtimeGroupPaletteHash` motion counter，确认 GPU 上传前 hash 连续变化。
   - 若视觉仍静态，下一步只查 `War3GetOrCreateSemanticShadowPalette` / shadow palette upload/cache，不再重开 owner 或 CModel+0x60 路线。

## 2026-05-01 07:12 +08:00 - skinned ShadowMap reuse gate 修正

1. 本轮目标
   - 用户继续反馈：性能可接受，但阴影仍像初始化姿态；单位移动时阴影位置约 3 秒更新一次，期间会短暂消失。
   - 优先解释“为什么位置和 pose 都像被 3 秒采样一次”，并保持 `objectFallbackDrawCount=0`。

2. 新增硬结论
   - `ShadowRendererCore` 和 submit packet 层的 palette hash 已经持续变化，数据层不是完全静态。
   - 真正的新漏洞在 ShadowMap 复用判定：`dynamicPoseSignature / dynamicSkinnedOutputCount` 只在 `unitLikeObject` 分支累加；formal semantic 路径里部分 skinned caster 仍可能是 `ObjectKind::Unknown`，导致 ShadowMap 把动态蒙皮 caster 当成静态场景复用。
   - 这能同时解释“太阳/receiver 仍动，但 caster 阴影位置和姿态几秒才刷新一次”。

3. 代码修正
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：
     - 只要 `skinned || usesDynamicMeshPositions || unitLikeObject` 就累加 dynamic pose signature。
     - skinned dynamic hash 显式纳入 `submittedRuntimeGroupPaletteHash`、palette count 和 `maxVertexGroupSlot`。
     - `dynamicSkinnedOutputCount` 不再依赖 `unitLikeObject`。
   - `War3ShadowReceiverPass::Run`：
     - ShadowMap adaptive reuse 增加 `hasDynamicSkinnedCasters` gate。
     - 只要 `dynamicSkinnedOutputCount / semanticSceneSubmittedSkinned / capturedUnitVertexBlend` 任一非零，就禁止复用上一帧 ShadowMap。

4. fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 低压图 perf：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_07_21.html`
     - `avgFps=84.032`
     - `dynamicSkinnedOutputCount=66296`
     - `semanticSceneSubmittedSkinned=66296`
     - `semanticSceneSubmittedPaletteMotionChangedCount=66279`
     - `semanticSceneSubmittedPaletteMotionStableCount=0`
     - `objectFallbackDrawCount=0`
     - `ShadowMap callsPerFrame=0.999`
   - 连续视觉序列：`AutoTest/artifacts/codex_dynamic_pose_sequence_20260501/pose_seq_01.png` 到 `pose_seq_06.png`
     - 同一隔离桌面窗口化场景下，女妖/火凤凰动作与对应阴影 silhouette 均发生变化。
     - 本轮未复现 3 秒阴影消失；War3 结束时已强制退出并关闭隔离桌面。

5. 当前 blocker
   - correctness 侧：动态 skinned ShadowMap 已不再被静态复用 gate 吞掉；仍需用户前台确认“单位脚下 shadow alignment 是否满意”。
   - performance 侧：禁止动态蒙皮复用后低压 FPS 回到约 84，属于正确性优先的中间态；下一轮需要做按 `runtimeGroupPaletteHash` 的安全复用，而不是全局跳过 ShadowMap。

6. 下一轮精确入口
   - `War3ShadowReceiverPass::Run`：把 ShadowMap reuse 从“有 skinned 全禁用”升级为“按 `dynamicPoseSignature`/palette hash 未变才复用”，同时保证动画 pose 变化时必重绘。
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：继续拆 `PaletteIndex`/upload cache 成本，避免每个 skinned packet 重算完整 palette。
   - 视觉：继续用 `codex_dynamic_pose_sequence_20260501` 方式做多帧序列，不再只用单张截图判断动态 pose。

## 2026-05-01 07:26 +08:00 - dynamic pose signature 纳入 world transform

1. 本轮目标
   - 接续用户反馈的“阴影位置约 3 秒更新一次、姿态仍像初始姿态”问题，避免后续为了性能恢复 ShadowMap reuse 时再次漏掉移动单位。
   - 保持 `objectFallbackDrawCount=0`，不恢复 VB/IB snapshot/freeze。

2. 新增硬结论
   - 上一轮修复后，动态 skinned packet 已会更新 `dynamicSkinnedOutputCount` 与 `dynamicPoseSignature`，且多帧截图确认 silhouette 会变化。
   - 但原 dynamic hash 对 skinned 分支主要纳入 runtime/palette/pose hash，未显式纳入 `draw.worldMatrix` 与 bounds；如果未来只用 `dynamicPoseSignature` 恢复 ShadowMap reuse，仍可能在“姿态暂时稳定但单位移动”的场景复用旧 shadow map。

3. 代码修正
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：
     - skinned / dynamic mesh / pose-hash 分支的 `dynamicHash` 现在显式纳入完整 `draw.worldMatrix` 四行、`boundsCenter` 与 `boundsRadius`。
     - 这样 `dynamicPoseSignature` 同时覆盖 animation palette 与 world-space placement，后续 ShadowMap 安全复用可直接依赖该 signature，不再只依赖 caster count/camera delta。

4. fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 低压图短窗：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_20_48.html`
     - `avgFps=67.720`（短窗启动阶段偏低；本轮主要做 correctness 验证）
     - `semanticSceneSubmittedSkinned=30732`
     - `dynamicSkinnedOutputCount=30732`
     - `objectFallbackDrawCount=0`
   - 连续视觉序列：`AutoTest/artifacts/codex_dynamic_pose_sequence_after_worldhash_20260501/pose_worldhash_01.png` 到 `pose_worldhash_06.png`
     - 6 张隔离桌面窗口化截图均成功；summary 每张对应 `semanticSceneSubmittedSkinned=34`、`semanticSceneSubmittedPaletteMotionChangedCount=34`、`objectFallbackDrawCount=0`。
     - 未复现 3 秒 ShadowMap 空窗；War3 已直接强制关闭并清理隔离桌面。

5. 当前 blocker
   - correctness：当前自动化序列看到了动态 silhouette，但仍需要用户前台确认不同单位脚下 alignment 是否符合预期。
   - performance：为保证动态 skinned 正确，ShadowMap 仍被保守地每帧重绘；下一轮优化只能基于已补齐 world transform 的 `dynamicPoseSignature` 做安全复用。

6. 下一轮精确入口
   - `War3ShadowReceiverPass::Run`：将 `!hasDynamicSkinnedCasters` 粗 gate 改为 `dynamicPoseStable` gate，但必须保留 world matrix 已进入 signature 这一前提。
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：继续拆 `War3GetOrCreateSemanticShadowPalette` 与 palette upload cache 成本。
   - 视觉：继续以多帧序列而非单帧截图确认动态姿态，不再用单张图误判。

## 2026-05-01 07:38 +08:00 - ShadowMap safe reuse 恢复

1. 本轮目标
   - 在上一轮将 world transform 纳入 `dynamicPoseSignature` 后，恢复一部分 ShadowMap adaptive reuse，但不允许重现“移动单位 shadow 位置几秒才更新”的旧问题。

2. 代码修正
   - `War3ShadowReceiverPass::Run`：
     - 将 “有 dynamic skinned caster 就完全禁用 reuse” 改为 `dynamicContentStable`。
     - 当没有 dynamic skinned caster 时按旧静态规则允许复用。
     - 当存在 dynamic skinned caster 时，只有 `dynamicPoseSignature != 0` 且与上一帧一致才允许复用。
   - 该逻辑依赖 07:26 修正：signature 已包含 palette/pose 与 world matrix/bounds。

3. fresh artifact
   - 构建：`ninja -C build32 -j4` 通过。
   - 低压图短窗：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_32_41.html`
     - `avgFps=68.166`
     - `semanticSceneSubmittedSkinned=35560`
     - `dynamicSkinnedOutputCount=35560`
     - `objectFallbackDrawCount=0`
     - `ShadowMap callsPerFrame=0.583`
   - 连续视觉序列：`AutoTest/artifacts/codex_dynamic_pose_sequence_safe_reuse_20260501/pose_safe_reuse_01.png` 到 `pose_safe_reuse_06.png`
     - 6 张截图均成功，control-plane summary 每次均为 `semanticSceneSubmittedSkinned=34`、`semanticSceneSubmittedPaletteMotionChangedCount=34`、`objectFallbackDrawCount=0`。
     - 未复现周期性 ShadowMap 消失；War3 已强杀并关闭隔离桌面。

4. 当前 blocker
   - FPS 仍低于用户期望，且本轮短窗的 `Other/UntrackedActive` 偏高；正确性优先修复已完成后，下一步应拆 CPU active gap 或降低 SubmitFrame/ShadowMap 成本。
   - 用户前台仍需确认不同单位脚下阴影 alignment；自动化截图显示 silhouette 会变化，但无法完全替代肉眼动态观察。

5. 下一轮精确入口
   - `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`：拆 `War3SemanticScene/SubmitFrame` 子阶段，优先看 `PaletteIndex` 与 palette upload/cache。
   - `war3_perf_monitor`/main loop scopes：解释 `Other/UntrackedActive` 13ms 级偏高是否来自测试窗口、游戏逻辑、或未标记渲染热段。
   - 继续保留 `objectFallbackDrawCount == 0` 和 `semanticSceneSubmittedSkinned > 0` 作为 no-fallback 验收线。
