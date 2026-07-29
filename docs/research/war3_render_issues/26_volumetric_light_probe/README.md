# CSM 同步体积光实验入口

## 目标

在不影响当前已收敛的 CSM 阴影、S1 地形遮挡、LOSBlocker 过滤与
UberSplat/HMED 保留策略的前提下，先建立一个可编译、可开关、可 AutoTest
验证的体积光/上帝光入口。

本轮只做实验骨架，不把效果默认打开。

## 实现边界

- 新增 `War3VolumetricLightPass` 接入 BeforeUi，构建为正式模块，但
  `War3VolumetricLightSettings::enabled=false`。
- 环境变量入口：
  - `DXVK_WAR3_VOLUMETRIC_LIGHT=1`
  - `DXVK_WAR3_VOLUMETRIC_INTENSITY=<float>`
  - `DXVK_WAR3_VOLUMETRIC_SAMPLES=<int>`
- ImGui 增加 `VolumetricLight` pass 开关与“体积光”参数面板。
- `War3ShadowReceiverPass::GetVolumetricShadowSnapshot(...)` 对体积光导出：
  - 完整 shadow map sample view；
  - 当前 CSM cascade 数据；
  - shadow map resolution；
  - 实际用于 shadow map 的 sun direction；
  - CSM 稳定化后的 worldUp。

关键原则：体积光不能自己重新猜太阳方向或世界上方向。它必须复用
ShadowReceiver 已经稳定化的 CSM 状态，否则会重新引入视角升高后淡化、
Y/Z 轴切换和方向漂移问题。

## 2026-07-09 正式化回写

本页记录的是第一版 full-res 实验入口。2026-07-09 后续实现已经把“生产启用前必须
低成本化”的问题落实为正式方案：

- `War3VolumetricLightPass` 不再直接写 full-res 颜色结果，而是先写低分辨率
  `R16G16B16A16_SFLOAT` effect buffer；
- effect buffer 存储 `rgb=shafts add color` 与 `a=column attenuation`；
- `war3_volumetric_composite.frag` 再把 full-res color copy 与低分辨率 effect buffer
  合成回最终画面；
- 默认 `resolutionDivisor=4`，需要更高质量时可通过环境变量或 JASS 命令改为 2/1；
- 新增 JASS command bridge：
  `set-volumetric-light-enabled`、`set-volumetric-light-params`、
  `set-volumetric-light-fade`、`set-volumetric-height-fog`、
  `set-volumetric-resolution-divisor`。

最新结论已迁移到
`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`。
该轮 AutoTest 中“点光源 + 体积光 divisor=4”短窗 `avgFps=51.058`，已接近
无点光/无体积光基线 `avgFps=51.247`。

## 与当前阴影问题的关系

- S1 terrain 仍是 CSM 的地形遮挡来源；S2 不是地形本体，不能作为体积光/阴影
  地形几何来源。
- LOSBlocker 过滤保持在 shadow caster 采集和最终 sweep 层，体积光只消费已经
  清理后的 shadow map。
- 建筑静态阴影继续由 UnitUI `buildingShadow(+0x50)` producer gate 处理。
- UberSplat/HMED 继续保留，不进入体积光治理范围。

## 验证

编译：

- `ninja -C build32` 通过。

默认关闭回归：

- 地图：`E:\Work\War3\Maps\ShadowTest\光影测试.w3x`
- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_23_48_02.html`
- 截图：`AutoTest/artifacts/screenshots/war3_20260708_234802.png`
- 结果：`ok=true`，无 `deviceLost / shadowCaptureIncomplete / csmComputeFailed`。
- 视觉：阻断器坡面小方块未恢复；狮鹫阴影未见明显切残。

轻量开启探针：

- 环境变量：`DXVK_WAR3_VOLUMETRIC_LIGHT=1`，
  `DXVK_WAR3_VOLUMETRIC_INTENSITY=0.12`，
  `DXVK_WAR3_VOLUMETRIC_SAMPLES=16`
- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_08_23_51_02.html`
- 截图：`AutoTest/artifacts/screenshots/war3_20260708_235102.png`
- 结果：`ok=true`，无 `deviceLost / shadowCaptureIncomplete / csmComputeFailed`。
- 视觉：未出现全屏爆亮、黑屏、阻断器阴影回归或地形遮挡污染。
- 性能：短窗 FPS 从默认关闭约 `62.6` 降到轻开约 `49.6`，当前成本偏高，
  因此默认必须继续关闭。

## 后续

1. 半/低分辨率方案已在 2026-07-09 落地为低分辨率 effect buffer + full-res
   composite，默认 `resolutionDivisor=4`。后续若继续压性能，再评估 temporal resolve。
2. 需要结合 War3 日夜系统做亮度曲线，避免夜晚或低太阳角度出现灰雾。
3. 需要继续用隔离桌面 AutoTest 做截图验证，尤其检查 UI 之前的雾、Bloom、
   SSAO 与 CSM receiver 叠加顺序。
