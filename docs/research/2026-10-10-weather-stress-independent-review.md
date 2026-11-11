# 2026-10-10 天气长门独立审查收口

本报告只记录本 lane 已实际检查的内容。生产、fixture 和 live contract 均未由本 lane 修改；没有编译、启动游戏或部署权限。按 root 收口指令，不再扩大实验。

## 1. 原 stress 读方的三项真实误通过

原 `weather_stress_contract_20261010.py` 配套纯测试独立复跑 **34/34，EXIT=0**，但以下在内存中对 `test_weather_stress_contract_20261010.make_receipt()` 的反例均真实误通过。每个反例都重新创建合成回执，未修改任何实机产物或合同文件：

| 反例 | 变更 | 当时结果 |
| --- | --- | --- |
| 好帧遮住账户增长 | stage34 第四份 observation 的 `render.resourceBytes += 1024`，同时 `render.frameSerial -= 1`；保留前三个好帧 | 全 required=True；`sameKeyCurrentWeatherBackingStable=True`；`noUnexpectedWeatherPreparationFailures=True` |
| 未知账户冒充零 | 全部 enabled 样本 `render.resourceBytes=None` | 全 required=True，same-key stable=True；`_integer` 默认回落 0，丢掉 unknown/known-zero 区别 |
| 错误冻结参数被筛走 | stage34 第四份 observation 的 `frozenInput.params.snowfall=0.2` | 全 required=True；`newInstanceParamsSurviveOldIdRefusals=True`；当前 owner 正常，但错误冻结输入被 phase admission 排除 |

这证明原读方的强结论超过了谓词覆盖，不能用“有三个好帧”掩盖同阶段已知坏样本。反例属于 CPU/解析器证据，不说明同样缺陷已经发生于游戏或 GPU。

runtime lane 获 root 授权后修复合同和测试，并回报三项被封堵。本 lane 随后独立复跑该新版纯测试，**39/39，EXIT=0**；新测试中包含上述三项名字对应的回归。这里认证的是该测试文件独立运行成功，不是已完成新版所有判断的逐行审计或 stress-b 原始样本全量复算。

约束没有改变：32 次关闭后 CPU 消费停用观察、32 次开启各至少三个 distinct actual frames、八次重建的旧 id 拒绝、实际参数与身份/帧域证据仍须满足。不能把停用诊断叫 GPU 全部退休，也不能把同 key 当前 backing 稳定叫进程无泄漏。

## 2. fixture、启动分支和证据等级

已读实际 stress JASS、纯 classifier 与 base runner `--suite stress` 分支：

- fixture 有 32 个 enable/off 周期、八次 destroy/create；旧 id 写入使用合法但不同的参数，不是靠非法参数制造拒绝。每次 play 立即查询时间，off 阶段查询此前时间是否真实推进。
- prepare 固定 suite/harness/API/script/map 身份，run 拒绝错误 suite 或准备后漂移的 stress contract；候选 DLL 固定显式哈希。
- 状态采样以 JASS stage 前后相同作 phase bracket；capture 也有前后状态与 stage 见证，但没有截图精确 weather frame 见证。
- startup 的 stage=-1 不纳入阶段覆盖，空回执不能通过 required。启动异常会进入 root 的 finally 回执流程；本 lane 没有模拟完整 launch fault 路径，不能给其每个异常分支签全覆盖。
- finally 保留 stop、玩家保护输入哈希及只读 video registry 检查；cleanup/身份/registry 错误将 acceptance 置 false 并先写 run.json，再报错。registry 读取失败是 unknown，不伪造 unchanged，不自动覆盖或恢复 registry。
- `collect_health` 对无事件只声明“所收集证据无故障关键字/incident”；GPU correctness 始终 false，历史淘汰不在覆盖内。health 不可得不是干净 GPU 证明。
- stress 的五个 NON_ACCEPTANCE 项始终保留 false：ordinary 当前资源账户、GPU 完成、像素/艺术接受、进程无泄漏、GPU 资源重建。runner 从 required 中排除它们，有意只验可支持结论，不是把它们改成通过。

另确认 stress capture classifier 不校验文件 path/SHA/size；合成 fixture 本来不带这些身份也能通过 phase-bracket 谓词。因此其名字只应解释为“七个捕获阶段前后状态吻合”，不能据此宣称七张文件存在/字节有效，更不能证明截图精确同帧。base runner 实际另行记录文件尺寸、SHA 与分辨率；最终打包应读取那些实际回执，而不是扩张 classifier 名称的含义。

## 3. stress-a 的实际边界与 root 后续回执

本 lane 只读了原 stress-a `run.json`：候选 **04E7137C54D83645D0AD1A7BFCF68BEE18E7EB60B1FC8FD52983CDDB432A3DF4**，500 份状态样本、70 个阶段回执、七张 capture，最后采样 elapsed **397.458 秒**。

该原轮 `runtimeAcceptance=false`；`allStressJassStagesAndChecks` 和 `newInstanceParamsSurviveOldIdRefusals` 为 false，原产物不能被新版合同追认为当时通过。其原读方的 32 off/32 recovery/当前 backing 结果为 true，但受第 1 节读方覆盖缺陷限制。实际 stop.ok=true，保护输入未变、video registry 未变；这不是无限期或 GPU 结果认证。

收口时 root 补充：生产 freeze **8A1565…**，新实机功能 **35/0**，stress-b **1083/0**，全部最终回归通过。**这些是 root 回执，不是本 lane 独立认证**。本 lane 未在收口前完成 stress-b 原始 resource/frozen 参数的全量复算，也未完成最终打包清单核验。

## 4. 两张新尺寸候选静帧

只各查看一次以下两张 2560×1440 未审查原图，没有重复打开旧图：

- `E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/artifacts/weather-night-20261010/stress-a/captures/sun-weather-without-ordinary-fog.bmp`
- `E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/artifacts/weather-night-20261010/stress-a/captures/middle-weather-recovery.bmp`

它们绑定 stress-a 的 **04E7137…** 候选，不继承给 8A1565… 的新 Froxel 候选。与此前大软圆相比，这两张的雪片不那么抢主体，建筑、单位、血条与原版顶部/底部 UI 可读；建议保留已做的 `size 1..3`，不要在本次收口继续叠近距 fade 或更多美术变动。

限制：stress 的 snowfall=0.65、haze=0.45 等，与旧 c 的 full 参数/普通光设置不同，不是严格同参数 A/B，不能把观感差异定量全部归因于 size。两张静帧不能证明运动连续、透明 portal 遮挡、完整音乐渐进、全部美术通过或玩家接受。

## 5. Froxel viewport 11 行变更的源码检查

检查对象：

`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/subprojects/war3fx/shaders/war3_volumetric_froxel_integrate.comp`

已读 diff、重建函数/main/integral/guide 发布相关代码，并对照 inject 与 temporal 的坐标产生/历史采样方式。结论如下：

- inject 用 grid cell normalized UV 直接构造相机 clip ray；temporal 也用 projection-local UV 重建和采样历史，所以 3D volume 是 viewport-local 相机网格，不是完整 RT UV 网格。
- integrate 先由 full-RT 输出 UV 算 `fullPixel`，再得到用于 depth/ray 的 `uvLocal`。将这个同一 `uvLocal` 作为 `volumeUv` 传回，用于 s_volume 查询，方向正确：不再用另一条 full-RT UV 列的密度去积分当前相机 ray。
- Y 方向保持 `1-2*uv.y` 的相同投影约定，没有新增翻转。已有 viewport outside 拒绝和 depth extent clamp 未改；重建返回 false 时不会使用未赋值的 volumeUv。没有 ABI、descriptor、push 布局或资源容量变化。
- guide 的屏幕 UV/输出像素、base-guide 查询、refinement 条件及失效写入未改，不能把 volumeUv 用于这些 full-RT 输出域；当前 diff 没犯这个域替换错误。
- 正确采到空间密度后，scattering/transmittance 与光学 shadow guide 的数值可能随之变化，这是修复范围的一部分。不能承诺“guide 像素完全不变”或“普通体积雾没有任何视觉变化”。

本 lane 独立复跑 `test_war3_weather_volume_static.py` **17/17，EXIT=0**，它是源码接线和数值模型证据，不是 GLSL/GPU 执行。全 viewport 也保留原 D3D half-pixel/整数 depth-pixel anchor；不能笼统声称每一种非整数缩放布局的新旧 volume UV 严格逐位相同。

未完成：最终 8A1565… 实机 viewport 原始证据独立回算、边界 scissor/远缩放/透明/FOW/运动连续/普通体积光全组合的视觉复审。本 lane 没有在这 11 行 diff 中确认新的 P0，但这不替代这些未做门。

## 交付上限

已交付三项读方反例、修后纯测试独立运行、限定启动/清理合同检查、两张新 size 静帧观察与 Froxel 域源码审查。未修改生产/合同/测试，未构建、未实机、未部署或发布。原失败产物保留，root 的新通过回执与本 lane 亲验结果分别列明。
