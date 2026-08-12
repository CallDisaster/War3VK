# 2026-08-12 局部体积雾与 clear-air 体积阴影候选

## 结果

本候选在既有体积太阳光/点光 pass 上增加了局部空间介质层，支持地图作者创建最多 8 个
Sphere、Box、Cylinder 体积。独立全局介质开关或全局 volumetric density=0 时，局部体积仍独立
工作；体积外没有介质，因此该路径可表现“角色遮光后的暗体积柱清楚，但周围空气基本清澈”。

实现遵循 [研究与公式记录](../research/2026-08-12-local-volumetric-fog-and-clear-air-shafts.md)，
没有复制 Unreal Engine 源码或第三方实现。

## 实现摘要

- `War3FogVolumeManager` 负责有界输入、句柄、形状变换、保守 bounding sphere 与不可变帧快照。
- fragment shader 把射线解析地裁剪到标准 Sphere/Box/Cylinder，再按每个固定 march segment 的
  真实重叠长度积分；局部体积小于一步时不依赖恰好命中中点。
- 新增 528-byte scalar-layout UBO，使用与现有 CSM/point UBO 相同的 24 槽 ring；binding 6 的
  C++ / GLSL ABI 由静态断言和测试锁定。
- 总消光为全局介质与所有局部介质之和；太阳使用既有 CSM，点光使用既有 cube shadow。
- CPU 先做视口保守剔除；shader 固定最多 8 个体积。除既有 4,000,000 ray-segment watchdog 外，
  局部介质交互另有 96,000,000 上限，并计入点光数量。
- JAPI 新增 `WARVK_FEATURE_LOCAL_FOG (0x8000)` 和完整创建、销毁、启停、位移、旋转、密度、
  feather、尺寸与存活查询。JASS 和 YDWE action/call/define 同步接通，不显示未实现菜单。
- JAPI/JASS/YDWE 新增 `WarVKSetGlobalVolumetricMediumEnabled`；它只开关均匀全局介质，保留
  density 作者值与局部体积。旧 `WarVKSetGlobalVolumetricFogEnabled` 继续只表示高度分布开关。
- CSM 阴影柱不再按摄像机俯仰改变探针间距、对比指数或底图加深上限；固定 10 世界单位探针间距，
  并用同源 reference/physical visibility 差乘实际光学厚度作为有界局部遮挡证据。
- 全局介质关闭时，小屏幕局部体积可走 conservative ceil-half scissor ROI；只有作者请求的完整
  sample count 同时满足 4,000,000 ray-segment 与 96,000,000 interaction 预算才启用，否则回退
  divisor=4..8 全屏路径。
- 地图会话 reset 在 Present 安全点清空 manager；JASS reset 同步销毁托管 registry 条目。GPU UBO
  仍由渲染 pass / 设备生命周期拥有，不由 Hook 线程释放。
- clear-air 候选默认改为 density 0.22、extinction 0.05、weight 2.10、intensity 1.20，关闭全局
  height fog，并把 unshadowed fallback 与 near presence floor 降到 0.03。

## 离线验证

- 全量静态脚本：77/77。
- Win32 runnable：21/21，其中新增 `war3_fog_volume_manager`，并扩充 JAPI protocol test。
- `build32_safe.cmd src/d3d9/d3d9.dll -j8`：通过，GLSL/SPIR-V 与 DLL 均重新生成。
- `ninja -C build32 -n src/d3d9/d3d9.dll`：no work。
- `git diff --check`：无新增 whitespace error（只有仓库既有 LF→CRLF 提示）。
- DLL：34,161,787 bytes；SHA-256
  `EA3D6E636B99E88FA4C7AEDF9DEEDD1FEEEC139EDC4504030823BE37BAB203D8`。

## 未授权/未完成的门

本轮没有部署 DLL、覆盖 Warcraft/YDWE 文件、启动编辑器或游戏，也没有执行真实
`UiCatalog::Load` 回读。以下结论仍需玩家前台物理门，不能由离线验证冒充：

1. Destiny 2 参考所需的暗柱连续性与周围空气清澈程度；
2. Sphere、旋转 Box、旋转 Cylinder 的边缘 feather 和尺寸直觉；
3. 角色/建筑 CSM 及点光 cube shadow 在局部体积中的视觉正确性；
4. 新默认值在天空、地形、低镜位和高曝光场景下的 A/B；
5. 单地图长巡航的 GPU 时间、budget、device-lost 与 incident 数据。

局部雾当前接收实体阴影，但不会形成可遮挡其他雾体积的三维 extinction shadow；该能力需要
后续 froxel extinction / lighting volume。既有跨地图和点阴影摩尔纹已知问题也不因本候选而视为修复。
