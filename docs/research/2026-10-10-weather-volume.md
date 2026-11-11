# 2026-10-10 暴风雪独立雪雾接线回执

范围：独立天气体积介质、普通体积光隔离、共享密度函数、Legacy 回退及诊断。
本 lane 未编译、未部署、未启动游戏；构建与实机由主线程唯一整合。
源码检查和数学反例不是 GPU 执行、雪雾美术效果或帧耗时验收。

## 玩家和作者会看到什么

`haze` 控制区域内的远景雪雾，`lowSnow` 控制靠区域底部更浓的低空吹雪。
`gust` 影响随连续风位移平移的条带密度；三项与雪片读取同一个按值天气帧快照。
首版为 Z-up 室外矩形体积，不是自动地形贴合、建筑避雨或屋檐碰撞。
底部是作者给定的水平高度：不声称各坡地都能得到精确贴地吹雪。

天气使用有限冷色环境散射 `vec3(0.30, 0.34, 0.40)` 和透射衰减，
不依赖太阳、点光或已有局部雾才能显示，也不替作者修改普通雾/光照配置。
当前政策没有天气阴影、逐灯散射或额外方向阴影 guide；这些不是本批已实现能力。

## 为什么必须有独立 owner

原普通体积光只有一套质量相关图像和缓存。若在同一 owner 中依次跑普通 High、
天气 Medium，下帧又回到 High，将反复改变三维图像尺寸并可能每帧重新创建资源。
扩大 UBO ring 不能解决这类质量切换与图像布局/历史的同帧别名。

本批在普通 `War3VolumetricLightPass` 内懒创建一个 weather-only 同类 owner：

- 普通 owner 继续用 `input.settings->postFx.volumetricLight`，配置不被天气改写。
- 天气 owner 使用独立 FroxelMedium 设置、独立图像、独立 24 槽 UBO ring、
  独立 owned-layout 状态、独立历史与后端日志状态。
- 两次执行借用同一个 `War3PipelineInput` 和按值 `input.weather`，不克隆 scene/caster vector。
- 只有有效帧、地图/设备代际、相机、颜色/深度目标和正 haze/lowSnow 才创建天气 owner。
- 从未创建天气时，不分配这一套天气资源。创建后关闭会提前返回，不再 copy/dispatch；
  资源保留用于以后恢复，不在每帧、JASS destroy 或换图时直接销毁 raw pipeline。

这消除了普通 High/天气 Medium 在单一缓存内来回重建的结构问题，
不是“免费复用同一 pass”：天气仍然是额外 GPU 工作和额外资源。

## 普通雾与天气设置的所有权

天气设置强制为：FroxelMedium，intensity/weight 为 1，普通 global density 为 0，
globalMedium/heightFog/pointLights/CSM-shadow/directional-guide-refine/temporal 均关闭，
样本上界 16，非自适应，effect divisor 8，独立 extinction 为 1。
普通局部雾列表和点光管理器只由普通 owner 消费。
天气 UBO 四个 vec4 只在 weather-only owner、三域身份匹配时填入；普通 owner 尾部保持零。
CPU fog UBO 为 592 bytes；Froxel inject 与 Legacy shader 的字段顺序相同。
四个尾字段有真实 C++ offsetof 静态断言：528/544/560/576 bytes，
不是仅在 Python 中猜测字段布局。

旧工作帧不能消费另一帧的天气：frameSerial/mapEpoch/deviceEpoch 必须逐项相等且非零。
天气 owner 同一帧/地图/设备 tuple 重复调用在 copy/update 之前退出。
`historyRevision` 加入天气历史有效性判据，seek/关闭会失效历史；首版天气 temporal 仍关闭，
不把历史 revision 检查当成玩家拖动时间线的完整视觉验收。

## 密度与连续风场的两条反例修复

区域 feather 仅对 XY 边缘和顶部生效。若 Z 两端都 feather，最低处密度为零，
会擦掉 `exp(-(z-bottomZ)/96)` 原本最浓的 lowSnow 层。
当前在区域底面中心 lowSnow=1、haze=0 的解析值为 0.0015，底面外仍为零。
顶部和 XY 边界在非零 feather 下仍渐隐；这不是突破作者区域边界。

雪片的风位移在 CPU 按 `8192*96=786432` 世界单位换环。
雪雾两波改为 `(2*pi/786432)*dot(positionXY-motionXY, integerWaveNumber)`，
波数分别 `(375,263)` 和 `(-151,711)`；X/Y 任意正负整周期平移密度不变。
体积 UBO 同步对两轴做 fmod786432，避免长时间风位移直接 float 化后的精度退化。
有限 float32 换环近邻数学测试允许数值误差，不声称 GPU 三角函数逐位一致。

## Legacy 是实际回退，不是空合成

Froxel 不支持/工作量门不准入时，Legacy shader 的天气分支仍重建世界射线，
先用显式平行轴 slab 求交裁剪到作者 box，再做最多 16 次中点密度积分。
透射为 `exp(-clamp(opticalDepth,0,2))`，颜色为独立冷色散射乘 `(1-T)`。
没有太阳/点光仍可执行该分支，不会把复制原颜色、零 effect 的空操作算作天气完成。
所有坐标、方向、域与 motion 非有限值 fail-closed；平行轴不靠除零制造无限区间。
薄区间裁剪后才分配采样点，仍然是有限解析近似，不是精确体积光求解器。

## 资源、成本和诊断边界

天气当前新增完整颜色/深度副本、低分辨率 effect/guide、三张 Medium RGBA16F 三维图像，
以及自己的三种 UBO ring。完整拷贝成本和双 pass 成本不能被“雪片无 CPU 模拟”掩盖。
天气最大目标尺寸 8192；图像分配前有独立 128 MiB nominal resource gate。
该门按源格式像素块、Medium 三维图像、effect/guide 和 UBO 估算，
不改变 Arena、Stage11/caster 完整性或既有 384 MiB 快照准入。
懒 owner 构造所需 pipeline/UBO 在图像门前，gate 不是精确驱动总 heap 硬上限。

`QueryWar3WeatherVolumeDiagnostics()` 单列天气执行阶段、退化原因、
`admittedShaderWork` 和 `resourceBytes`，不覆盖普通体积光执行/工作量诊断。
`resourceBytes` 累加当前持有 DxvkImage 和 DxvkBuffer backing 的 memoryInfo.size，
不包含驱动 pipeline/sampler 内存、allocator 额外占用、或已替换但 GPU 尚未退役的旧资源。
CPU `WeatherVolume` scope 可用于 CPU 成本测量；本 lane 未添加/测出独立天气 GPU timestamp，
不能用 CPU scope 值代替 GPU 时间，也不能将内部理论工作量当成实测 GPU 成本。

## 最终析构只读审计

天气 child 与普通父 pass 一起存活，不在禁用/seek/换图路径 reset/delete child。
当前 device 最终正常析构顺序为 Flush → SynchronizeCsThread → DXVK waitForIdle →
DestroyWar3RenderPipeline。后者销毁 pass unique_ptr，再销毁天气 child，
raw VkPipeline 的最终销毁在源码顺序上位于 GPU idle 之后。
RegisterPass 不替换已有同名 pass，SetPassEnabled 只修改 enabled。
模块卸载 body 提前返回不调用 pipeline delete。
此审计限定正常最终析构的源码顺序；真实 device-lost、进程卸载及退出压力门未由本 lane 验证。

## 追加 P1 修正：天气准备阶段分配异常已隔离

发现实际 P1：独立资源 owner 不等于独立异常边界。天气构造或 RunMedium 抛异常，
会冒泡到 War3Pipeline 的外层 catch，使整个 VolumetricLight entry.enabled=false，
从而也停用已经独立的普通作者雾。128 MiB nominal gate 不能排除驱动 OOM。
首次审查在 fresh build 冻结期间只报告；主线程暂停构建并授予有界补丁窗口后才修改。
现只在 parent 的天气构造和 child 的天气资源准备阶段捕获 DxvkError/bad_alloc。
失败/等待均单列具名诊断，不冒泡到整个作者 fog pass 的永久停用入口。
已锁存 DEVICE_LOST 仍重抛，交还原终态规则；record 阶段异常没有新增吞异常逻辑。

资源准备先于任何天气 world copy/UBO update/beginRendering：
ensureResources → 三张 Froxel image → fallback cube/view → Legacy 与 composite pipeline →
所有天气 view 的懒 descriptor 实体。fallback 资源分配和 clear 命令已拆开，
记录阶段仅清空已准备的 cube；天气图形/合成仅 find 已缓存 pipeline，
Froxel 使用 prepared 状态，不在绘制中补创建资源。

原 raw compute pipeline 的构造中途失败原本不会触发类析构。
现在构造 catch 与最终析构共用 noexcept releasePipelines：释放尚未被命令使用的 partial pipeline，
句柄归零并清理 map；图形 pipeline 已创建但 map insert 抛异常也释放未缓存新句柄。
Froxel 三图采用候选完整创建后成套发布，保留旧 Rc 所有者和在途命令引用。
普通作者 pass 的资源/record 异常边界不被天气改写。

parent/child 使用生产 `WeatherResourceRetry` 单槽无 heap 组件：
key 仅包含地图/设备代际、尺寸及颜色/深度格式，同 key 失败后至少等 120 业务帧，
新资源 key 可立即尝试，成功 clear。天气 id/参数/作者 revision 不绕过退避。
组件的 80 条 CPU 断言由主线程单独编译执行，本 lane 未自行编译。
资源 OOM 与真实 device-loss 注入、记录后异常恢复仍待主线程实机门，
不能从静态 source 合同推导 GPU 故障已全面解决。

## 实际验证与待验

- `python AutoTest/test_war3_weather_volume_static.py`：13/13、EXIT 0。
  其中 8 个初始接线合同、2 个带生产 GLSL 提取常量的数学反例，
  3 个资源异常隔离/准备顺序/partial 清理与 ABI 收紧合同。
- 主线程修订旧合同到追加四 vec4 与独立天气诊断后，本 lane 实跑
  `test_war3_local_volumetric_fog_static.py` 10/10、
  `test_v122_visual_api_static.py` 8/8；未自行修改老测试迁就实现。
- 数学反例覆盖：底面 lowSnow、顶部/边界、X/Y 正负整周期、正负 fmod 换环 float32 邻点；
  同时证明旧非周期公式在选定边界存在大于 0.05 的密度条带跳变。
- 本批相关生产文件 `git diff --check`：EXIT 0；Git LF/CRLF 提示不当成测试失败。
- 初版静态测试误用了 unittest 保留方法名 run，首次运行失败；改为 run_body 后才通过，
  未把失败运行追认为通过。
- 本 lane 不做编译；Shader/DLL 结果、实机画面、有限预算下合法效果恢复、
  普通 High 与天气 Medium 并用成本及长时/换图/退出结果由主线程后续记录。
