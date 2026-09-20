# 2026-09-14 玩家高压图报告与原生点光接入边界

状态：PLAYER_OBSERVATION / SOURCE_AUDIT；不是完整稳定版或新增灯光验收。

## 输入与性能

- 玩家报告：`E:/Work/Warcraft III/WarVK/Log/war3_perf_report_2026_09_14_09_43_36.html`。
  7,703,506 bytes，SHA-256
  `86CFB2A3DEB4528CD4781AC718DDE89460D5C412176738BA6A814210440F4C7A`。
- 报告绑定整晚交付 DLL：34,577,677 bytes，SHA-256
  `B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993`。
  strict const-data 根解析递归重复键为 0；full_default，disabledModules 为空。
- 玩家确认高压地图运行良好，主观比较约提升 40%。当前未提供配对的旧高压地图报告，
  因此记录为玩家观察，不重新计算或宣称独立同条件 A/B 收益。
- `analyze_frame_timeline.analyze(last=4096)` 闭合连续整数 QPC 账本：4096 个完整 Present
  区间平均 10.776938208 ms，倒数约 92.790733 FPS，同窗主线程 CPU 10.551453 ms。
  报告旧口径 4000 帧平均 10.447 ms / 95.720 FPS 不包含完整 Present 尾部，不混用分母。
- `analyze_native_frame_sync.validate(candidate=True,last=4096)` 通过：4096 calls、4096
  requestOne、4096 elided；requestZero/Other/capture/unreadable/nested 全 0，各帧对应
  backbuffer lock census 与 lockTicks 均 0。说明该采样窗同步优化确实启用，非完整截图功能验收。
- 报告 GPU pass union 平均 2.522 ms（旧报告窗），不可直接与完整账本相减。
  reported framesIncomplete/framesBudgetExceeded 均 0，完整 CSM 存在、4 cascades。
  此报告没有完整独立 GPU event/incident 取证，不能据此消除之前闪退或随机裂缝问题。

## 为什么火盆、火把没有 WarVK 点阴影

这是未完成的自动接入链，不是本轮已接通点光后发生的渲染回归。

1. 既有点光引擎支持显式 API/JAPI、调试或 UI 创建的灯。
   `war3_shader_api.cpp` 的 `AddPointLight` 注册成功后可开启点光及点阴影设置；
   `d3d9_war3_pipeline.cpp` 的消费条件还要求 `War3LightManager::HasActiveLights()`。
   单独勾选效果开关不会从游戏模型创建灯。
2. 原生模型接入仍是 Stage A 诊断：`war3_native_model_light_frame.h` 中
   `lightingAuthorized=false`、`shadowAuthorized=false`；bridge 没有把采集灯发布给
   `War3LightManager`。build32 已配置 `warvk_native_model_lights_dev=false`，本交付也未
   编入该原生灯观察入口；即使打开观察编译门，仍不等于开启产品消费。
3. 9 个路径、11 个版本的原生点阴影候选白名单保持 `runtimeAuthorized=false`。
   `FindNativeLightShadowCandidate` 只有定义和测试调用，没有产品消费；先前 MPQ 模型普查、
   布局逆向和白名单建立不是运行时接通或视觉验收。
4. 报告无 `PointShadow/...` section。persistent point-shadow 诊断字段全 0，但它们只覆盖
   指定 worker 路径；报告没有通用 active-light census，不能单凭这些字段断言所有灯数量为 0。
   VolumetricLight 的入口计时也不是实际点光或体积光执行证明。

发光材质、火焰粒子或贴图光晕不等于真实模型灯节点，也不自动产生点阴影；不得仅按火把名称造灯。

## 后续实现边界

- 将实际加载的模型资源身份/路径/内容、generation 与运行时灯节点正确关联；区分地图覆盖资源。
- 按已逆向证明的字段转换位置、颜色和衰减；原生 `CGxuLight +0x28` 为选择评分，不得当半径。
- 补地图/设备/模型生命周期与销毁，先验证有界点光消费，再让已核白名单进入有预算的点阴影。
- 明确原生 D3D9 灯与增强灯的替代/共存策略，防止双重照明；不能全局禁用 SetLight 来绕过问题。
- 新增实际 active/accepted/rejected/shadow-selected 数量及阶段耗时，单独做视觉与成本验证，
  不将其混入本次已观察到的同步优化收益。

本轮仅离线分析玩家报告、只读源码及追加文档；没有修改灯光代码、构建、部署、启动游戏或恢复 DLL。
玩家试用现场与暂停的自动测试保持不动，根 CHANGELOG 不新增稳定接受。
