# 2026-10-10 暴风雪作者接口回执

范围：暴风雪 v1 的 CPU 作者接口、JASS 包装、YDWE Catalog 和生产派发测试源码。
本回执不代表实机、视觉或性能验收。GPU、Froxel、管线及构建整合由主线程负责。

## 作者能控制什么

最多一个暴风雪实例。创建后默认关闭，可在地图初始化时配置，不要求活动设备或渲染 settings 就绪。
作者可以控制降雪、风向、风速、阵风、远处雪雾、低空吹雪，以及一个室外 XY 矩形和 Z 高度范围。
可以直接平滑切换目标天气，也可以添加最多 64 个关键帧，再随音乐播放时间线。
首帧天气 GPU 资源分配由渲染器所有者负责；接口本身不创建 GPU 资源、不读游戏裸指针。

## 十二个函数与协议

`i` 是完整有符号 int32，`d` 是正整数对象句柄，`r` 是有限实数，`b` 只接受 0/1。
所有命令沿 `warvk:v1`，严格要求参数数量、顺序、类型和承载原版函数匹配。

| 函数 | 命令 | Carrier | 参数签名 | 返回 |
|---|---|---|---|---|
| WarVKCreateSnowWeather | weather.createSnow | Hotkey | i | integer |
| WarVKDestroyWeather | weather.destroy | Preloader | d | nothing |
| WarVKIsWeatherAlive | weather.isAlive | Hotkey | d | boolean |
| WarVKSetWeatherRegion | weather.setRegion | Preloader | drrrrrrr | nothing |
| WarVKSetWeatherEnabled | weather.setEnabled | Preloader | db | nothing |
| WarVKSetSnowWeatherTarget | weather.setSnowTarget | Preloader | drrrrrrr | nothing |
| WarVKClearWeatherKeyframes | weather.clearKeyframes | Preloader | d | nothing |
| WarVKAddSnowWeatherKeyframe | weather.addSnowKeyframe | Preloader | drrrrrrr | nothing |
| WarVKPlayWeatherTimeline | weather.playTimeline | Preloader | dr | nothing |
| WarVKSetWeatherPaused | weather.setPaused | Preloader | db | nothing |
| WarVKSetWeatherTime | weather.setTime | Preloader | dr | nothing |
| WarVKGetWeatherTime | weather.getTime | LocalizedString | d | real |

关键参数顺序：

- Region：weatherId, minX, minY, maxX, maxY, bottomZ, topZ, edgeFeather。
- Target：weatherId, snowfall, windAngle, windSpeed, gust, haze, lowSnow, transitionSeconds。
- Keyframe：weatherId, timeSeconds, snowfall, windAngle, windSpeed, gust, haze, lowSnow。

四个强度 snowfall/gust/haze/lowSnow 为 0..1；风速 0..2048 世界单位/秒。
风向采用角度，0 沿 +X，90 沿 +Y，Z 向上；有限角度由 runtime 规范化，按最短方向插值。
区域位置绝对值 <= 1e7，三个最大值必须严格大于最小值，edgeFeather 为 0..0.5。
过渡时间 0..3600 秒，演出／关键帧时间 0..86400 秒。
范围与状态约束由生产 WeatherRuntime 再验证；语法错误使用既有精确错误码，非法范围或状态为 BackendRejected。

## 时间线行为

- 第一关键帧必须 0 秒，后续严格递增；播放期间禁止编辑。
- Play 自动开启天气，时间线最后保持最终状态。
- SetTarget 从当前求值切入手动过渡，停止时间线；不会隐式开启关闭的实例。
- SetEnabled(false) 隐藏天气并冻结演出时间，保留配置；重新开启后继续。
- SetPaused 保持画面参数与时间，恢复不补算暂停期间墙钟时间。
- SetTime 是显式定位，要求渲染历史按 revision 重建；不会自动取消暂停或开启实例。
- GetTime 返回天气演出时钟；并未读取音频设备的播放位置。
- 时间线与音乐由同一个地图触发流程协调，原版声音函数继续负责音乐。

示例：`WarVK/jass/warvk_weather_demo.j`。其 0/30/60/90 秒只用于演示，非柴六实际分段。
示例检查每一步配置错误并清理失败实例；不会自动启动或修改其他地图。

## 句柄、能力与换图

采用现有统一受管对象 registry，新增 `ManagedType::Weather=4`。
返回的 publicId 映射到 WeatherRuntime 内部 id，避免与点光、闪电、局部雾的数字句柄冲突。
通用 count/isAlive/type 查询纳入天气，并允许在渲染就绪前执行这些 CPU 查询。

新增能力位 `WARVK_FEATURE_WEATHER=131072`（0x00020000）和类型常量 `WARVK_OBJECT_WEATHER=4`。
未有渲染器时能力查询只报告确实存在的 weather/managed CPU 能力，其余仍遵守既有 ready 判定。
功能编入 DLL，无天气环境变量开关；地图没有创建并开启实例时不请求天气绘制。

本次收紧公开 id 生命周期：ResetAuthorState 清空 registry，但不把公开 id 计数重置为 1。
计数用 uint64_t，超过 INT_MAX 时创建 fail-closed，不发生 signed 溢出或回绕复用。
这防止旧地图迟到命令命中新地图新建对象，适用于同一统一 registry 的既有对象类型。
创建天气后若 registry 插入失败／抛异常，会销毁内部天气，防止失败创建占死唯一实例。
ResetAuthorState 调用天气 CPU reset；旧 queued 帧仍由各自按值快照及地图／设备代际规则处理。

## 测试边界

- `AutoTest/test_warvk_weather_api_static.py`：7/7，EXIT=0；核 C++/JASS/GUI 三侧顺序、类型、Carrier、类别、常量和示例错误检查。
- 既有 `test_warvk_japi_safety_static.py`：5/5，EXIT=0。
- 既有 integrated static 首跑 19/20：唯一失败是旧命令数硬编码 110，新表为 122；已报告主线程处理，不能标全绿。
- `AutoTest/test_warvk_weather_protocol.cpp` 已交付，待主线程构建与执行。

协议可执行测试使用 WARVK_JAPI_PROTOCOL_TEST 排除旧 GPU/native backend；天气 runtime、dispatcher、registry 和严格解析器均为生产同一份代码。
传统非天气对象在此宏下不伪造存活。生产仍通过原来的 point-light/lightning/fog backend 验证其存活。
测试覆盖全部 12 命令的载体、缺参／多参、标签错位，以及 seed bit pattern、字段透传、范围拒绝、
时间线定位与最短角度、64 容量、销毁／换图旧 id、generic registry 查询和 legacy 实数返回格式。
INT_MAX 耗尽路径有源合同检查，未运行 20 亿次创建；混合 native 对象与天气的实机查询仍待验证。

未编译、未部署、未启动游戏、未提交或发布。尚未证明音画同步误差、粒子遮挡、透明物件／战争迷雾顺序、
场景可读性、GPU 成本或跨地图物理恢复。首版室外，不含建筑积雪、屋檐避雪和复杂地形贴合。
