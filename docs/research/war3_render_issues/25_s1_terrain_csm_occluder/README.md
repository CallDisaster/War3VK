# S1 地形 CSM 遮挡体与阴影穿透修复

## 结论

2026-07-08 通过 IDA + 实机图 `E:\Work\War3\Maps\ShadowTest\光影测试.w3x`
复核，War3 阶段语义必须这样区分：

- S1：`CWorld_DispatchStage(1) -> TerrainShadowDispatch(0)`，这是主地形几何。
- S2：`CWorld_DispatchStage(2) -> TerrainShadowDispatch(1)`，内部是
  `TerrainShadow_RenderLayer(ListA=1,ListB=0,type=0)`，属于战争迷雾、阴影、贴花混合层，
  不是地形几何本体。
- S19：`CWorld_DispatchStage(19) -> TerrainShadowDispatch(14) -> RenderListB(type=4)`，
  可承载建筑地面贴花/UberSplat，例如国王祭坛 `HMED`，不能因为去阴影粗拦截。

因此 CSM 要解决“飞行单位阴影穿透悬崖到低层”的问题，只能把 S1 纳入遮挡体，不能把 S2
当作地形，也不能依赖 S19/ListB。

## 实现策略

代码落点：

- `src/d3d9/d3d9_device.cpp`
- `src/d3d9/d3d9_war3_shadow.cpp`
- `src/d3d9/d3d9_war3_shadow_resources.cpp`
- `subprojects/war3fx/shaders/war3_shadow_caster_mask.frag`
- `subprojects/war3fx/shaders/war3_shadow_receiver.frag`
- `subprojects/war3fx/shaders/war3_shadow_visibility.frag`
- `src/d3d9/war3/core/war3_internal_test_config.h`

关键开关：

- `kShadowSemanticCoreSceneKeepS1TerrainLegacyCapture = true`
- `kShadowCascadeCullTerrainWithBounds = true`
- `kShadowS1TerrainCasterMaskEnabled = true`
- `kShadowS1TerrainCasterDepthBiasEnabled = false`

方案分三步：

1. legacy draw-time ShadowCapture 只对 S1 terrain 放行；语义场景接管后，S2/S19 仍不会被当作 terrain caster。
2. 主 ShadowMap 保持 depth-only，把 S1 terrain 写入 CSM 深度图，让它能挡住高处单位/飞行单位阴影继续投到低层。
3. 紧跟一个 S1-terrain-only R8 mask pass，用同一张 shadow depth 做 `LESS_OR_EQUAL` 深度测试，只在“最近遮挡体确实是 S1 地形”的像素写 `mask=1`。

S1 terrain 级联剔除：

- terrain draw 不再无条件进入所有 cascade；
- draw-time 读取的 tile bounds 对静态 VB 使用 TLS direct-map cache；
- per-draw UP 上传路径会记录原始上传源作为 source-key，能命中时复用 bounds；
- 动态 slice/content-key 持久缓存已做专图实验，命中率只有约 1-2%，默认关闭，
  避免为了低命中率增加额外采样；
- 当前裁剪是性能优化，不改变 S1 作为遮挡体的语义。

S1 terrain mask pass：

- 主 ShadowMap 的完整 caster 排序仍保持不变；
- mask pass 预先收集 `stage=1 && category=Terrain` 的 prepared index，只遍历 terrain 小列表；
- 即使小列表为空仍按 cascade 清空 R8 mask，避免上一帧残留。

receiver 采样时：

- 如果 shadow depth 显示当前 receiver 被挡住；
- 且 caster mask 表明最近 blocker 是 S1 terrain；
- 则该点放亮，不把地形自身作为可见投影结果。

这样 S1 terrain 仍然能作为遮挡体解决穿透，但不会把悬崖、山体、地形 tile 自己投成大块暗斑。

## LOSBlocker 关系

路径阻断器不是 S2 静态阴影。实测它会进入 Stage 11/CUnit 类世界对象路径，CSM 中表现为：

- 有身份时：`YTfb/YTpb/YTab/...` FourCC 可直接拦截；
- 丢失身份时：会退化成 `rawcode=0 / jHandle=0 / stage=11 / vtx=4 / idx=6`
  的匿名小平面。

因此当前稳定策略是 FourCC/模型路径/对象桥接命中优先，最后用极窄的匿名小平面几何指纹兜底。

## 验证

- 编译：`ninja -C build32` 通过。
- AutoTest 地图：`E:\Work\War3\Maps\ShadowTest\光影测试.w3x`
- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_20_48_09.html`
- 截图：`AutoTest/artifacts/screenshots/war3_20260708_204810.png`
- 关键指标：
  - `avgFps=66.797`
  - `avgGpuTimeMs=4.659`
  - `ShadowMap avgGpuMs=1.479`
  - `semanticSceneReplayDrawsCount=140`
  - `semanticSceneShadowMapDrawnCasters=559`

2026-07-08 追加验证（S1 terrain static/source-key bounds）：

- 隔离桌面截图：`AutoTest/artifacts/screenshots/war3_20260708_210934.png`
- 全屏性能报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_21_12_57.html`
- 全屏截图：`AutoTest/artifacts/screenshots/war3_20260708_211258.png`
- 同口径 ShadowMap 指标变化：
  - `semanticSceneShadowMapDrawnCasters: 559 -> 452`
  - `semanticSceneShadowMapCascadeCulledCount: 3041 -> 52467`
  - `ShadowMap avgGpuMs: 1.479 -> 1.025`
  - `Shadow/Main avgGpuMs: 2.142 -> 1.666`

注意：全屏复测的整帧 `avgFps` 受 `Other/UntrackedActive` 主线程波动影响，从
`66.797` 变为 `41.594`，不能仅凭该项判断 S1 bounds 裁剪退化。模块级指标显示
ShadowMap GPU 和 caster 数量均下降；后续若要做性能结项，应增加同场景多轮中位数对比。

2026-07-08 后续专图复核（动态 terrain bounds cache 证伪）：

- `war3_perf_report_auto_2026_07_08_22_12_27.html`：UP source-key 未覆盖当前路径，
  低频统计显示 `sourceKey=0`。
- `war3_perf_report_auto_2026_07_08_22_21_11.html` 至
  `war3_perf_report_auto_2026_07_08_22_32_50.html`：尝试 mapping-sequence 与
  16 点 content-key 后，缓存命中率仍只有约 1-2%。
- 结论：不要把动态 terrain VB bounds cache 当成生产收益；当前默认保留静态
  slice / UP source-key 能力，关闭动态 content-key 实验。下一步若要继续提速，
  应从 IDA 里找 terrain chunk/tile 身份，而不是继续对重建中的 VB 做采样缓存。

2026-07-08 收口短测（UberSplat/type4 保护 + S1 terrain 回归）：

- 隔离桌面报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_22_58_23.html`
- 截图：`AutoTest/artifacts/screenshots/war3_20260708_225824.png`
- 结果：`ok=true`，`frameCount=872`，`framesIncomplete=0`，`framesBudgetExceeded=0`，
  无 `deviceLost / shadowCaptureIncomplete / csmComputeFailed`。
- 日志确认：
  - S1 terrain legacy capture 仍被保留；
  - `YTfb/YTpb/YTab` 等带 FourCC 阻断器被拒绝；
  - rawcode=0 的 `stage=11 / vtx=4 / idx=6` 匿名小平面继续由
    `AnonymousSmallFlatMarker` 兜底拒绝。
- 视觉确认：专图坡面没有恢复密集小方块；狮鹫阴影未出现明显切残。

2026-07-08 追加短测（mask pass 预筛）：

- 隔离桌面报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_21_28_07.html`
- 截图：`AutoTest/artifacts/screenshots/war3_20260708_212808.png`
- `semanticSceneShadowMapDrawnCasters=419`
- `semanticSceneShadowMapCascadeCulledCount=33275`
- `ShadowMap avgCpuMs=0.432`
- `ShadowMap avgGpuMs=1.158`
- 无 `deviceLost / shadowCaptureIncomplete / csmComputeFailed`。

日志确认：

- FourCC 阻断器命中：`YTfb/YTpb/YTab`
- 匿名小平面命中：`rawcode=0, stage=11, vtx=4, idx=6`
- 无 descriptor 崩溃、无 incomplete shadow map。

视觉确认：

- 阻断器坡面不再出现小方块阴影。
- 狮鹫阴影未被 terrain depth bias 切残。
- 悬崖/高低地形的多重投影明显收敛。

2026-07-08 quiet-log 回归：

- 隔离桌面报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_23_21_26.html`
- 截图：`AutoTest/artifacts/screenshots/war3_20260708_232127.png`
- `ninja -C build32` 通过。
- 默认关闭以下取证日志，避免日常 AutoTest 被输出通道干扰：
  - `DXVK_WAR3_SHADOW_DRAW_SURVEY`
  - `DXVK_WAR3_CASTER_COMPOSITION`
  - `DXVK_WAR3_VB_ALLOC_SPIKE_LOG`
  - `kShadowCaptureStageProbeLogging`
- rawcode=0 匿名小平面逐项拒绝日志也改为 `kPathBlockerDebugEnabled`
  下输出；清扫逻辑本身保持默认开启。
- 本轮日志无 `deviceLost / shadowCaptureIncomplete / csmComputeFailed`，坡面阻断器
  小方块未恢复，狮鹫阴影未见明显切残。

## 后续性能项

二阶段 mask 已避免“所有 caster 都写 R8 color attachment”的高成本路径；S1 bounds
裁剪可减少部分远 cascade 重复绘制，但动态 VB 持久缓存已证伪。后续若要继续压性能，
优先方向是：

1. 用 IDA 继续找 terrain 原生 chunk/tile 元数据，给 S1 terrain draw 一个稳定身份。
2. 做同场景多轮中位数 AutoTest，剔除 `Other/UntrackedActive` 波动后再判断整帧收益。
3. 保持动态 slice/content-key cache 默认关闭，除非后续有更稳定的原生 key。

不要恢复 depth bias。实机已经证明它会切掉飞行单位的大阴影，视觉风险高于收益。
