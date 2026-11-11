# 2026-10-10 暴风雪开发工作单

授权：作者于本线程批准开始实现可编排暴风雪 v1，开发截止台北 2026-10-10 09:00。
仓库：E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk。
主分支：codex/v1.22-main-tree-20260920。现有 dirty/untracked 属于作者，保留全部原改动。
协调者：本 Codex /root；使用 Codex 子代理的消息回执。旧 DSH 夜班的日期、B 树及父线程 ID 不适用于本单。

## 目标与时间

- 12 个天气 JAPI + YDWE 分类、固定容量 CPU 时间线、统一帧天气快照。
- 有界 GPU 雪粒子、Froxel 雪雾/低空吹雪、统一风向；默认未创建实例时关闭。
- 源码/生产 CPU/Shader 编译/DLL 构建与必要回归逐项记录，实机及视觉按实际结果报告。
- 首版一个天气实例，最多 64 个关键帧；作用区域为世界 XY 矩形及 Z 高度。
- 08:20 后不新增功能批次；08:30 停止写入并收尾；09:00 前保存报告。
- 室外场地区域；建筑自动覆雪、厚积雪、屋檐碰撞和精确坡地贴合延期。

## 权限与分工

各 lane 先读本单。只编辑白名单。使用 apply_patch。不得改系统/玩家目录、提交、推送、发布或删除原证据。
编译和游戏只由主线程授予 lease。总构建并发最多 -j2，BelowNormal。仅隔离零全局输入测试。
完成后发送 Codex 父代理回执：文件、接口、实际测试/未测试、风险、下一步；停止写入等待指示。

| lane | 写入白名单 | 交付 |
|---|---|---|
| runtime | src/d3d9/war3/render/war3_weather_runtime.h/.cpp; AutoTest/test_war3_weather_runtime.cpp; docs/research/2026-10-10-weather-runtime.md | 有界作者状态、时间线、时钟、快照、生产组件反例 |
| api | src/d3d9/war3/japi/war3_japi_v1.cpp; WarVK/jass/warvk_api.j; WarVK/action.txt; WarVK/call.txt; WarVK/define.txt; WarVK/jass/warvk_weather_demo.j; AutoTest/test_warvk_weather_api_static.py; docs/research/2026-10-10-weather-api.md | 12 函数与 Catalog、严格校验、换图作者清理 |
| render | src/d3d9/d3d9_war3_weather.h/.cpp; subprojects/war3fx/shaders/war3_weather_snow.vert/.frag; AutoTest/test_war3_weather_shader_contract.py; docs/research/2026-10-10-weather-render.md | GPU 实例化雪粒子、遮挡、固定预算、CPU/GPU 寿命 |
| root | 所有共享 Meson/device/pipeline/volumetric/perf 接线、生产协议测试、整合门禁、开发日志和最终报告 | 唯一整合与构建所有者 |

## 共同接口

namespace dxvk::war3::render。
SnowWeatherParams 六个 float：snowfall, windAngle, windSpeed, gust, haze, lowSnow。
WeatherRegion 七个 float：minX,minY,maxX,maxY,bottomZ,topZ,edgeFeather。
WeatherFrameSnapshot：params, region, frameSerial/mapEpoch/deviceEpoch/revision (uint64_t),
timeSeconds (double), windDisplacementX/Y (double), seed (uint32_t), id (int32_t), enabled/paused (bool)。
字段命名严格固定；snapshot 含 active id，零/disabled 表示无需天气资源。

WeatherRuntime::instance()。
public methods：
- int32_t createSnow(uint32_t seed)
- bool destroy(int32_t id), isAlive(int32_t id)
- bool setRegion(int32_t id, const WeatherRegion&)
- bool setEnabled(int32_t id, bool)
- bool setTarget(int32_t id, const SnowWeatherParams&, float transitionSeconds)
- bool clearKeyframes(int32_t id)
- bool addKeyframe(int32_t id, float timeSeconds, const SnowWeatherParams&)
- bool playTimeline(int32_t id, float startSeconds)
- bool setPaused(int32_t id, bool)
- bool setTime(int32_t id, float seconds)
- bool getTime(int32_t id, float& outSeconds)
- WeatherFrameSnapshot snapshot(uint64_t frameSerial,uint64_t mapEpoch,uint64_t deviceEpoch)
- void resetAuthorState()

为生产组件测试允许构造 WeatherRuntime(Clock clock)，Clock=double(*)() noexcept，单位为单调秒；生产默认 steady_clock。
构造实例无堆分配，snapshot 固定按值，不读游戏裸指针；锁保护 CPU 作者状态，每帧一次快照。
render lane 使用 input.weather（WeatherFrameSnapshot 按值成员，root 接线）。禁止自行读 live WeatherRuntime。
帧内 shared snapshot；旧 queued command 的值不可被后续 JASS 改写。根在获取帧输入时冻结。

## 作者 API 和行为

- WarVKCreateSnowWeather(integer seed) returns integer
- WarVKDestroyWeather(integer id) returns nothing
- WarVKIsWeatherAlive(integer id) returns boolean
- WarVKSetWeatherRegion(integer id, real minX,minY,maxX,maxY,bottomZ,topZ,edgeFeather) returns nothing
- WarVKSetWeatherEnabled(integer id, boolean enabled) returns nothing
- WarVKSetSnowWeatherTarget(integer id, real snowfall,windAngle,windSpeed,gust,haze,lowSnow,transitionSeconds) returns nothing
- WarVKClearWeatherKeyframes(integer id) returns nothing
- WarVKAddSnowWeatherKeyframe(integer id, real timeSeconds,snowfall,windAngle,windSpeed,gust,haze,lowSnow) returns nothing
- WarVKPlayWeatherTimeline(integer id, real startSeconds) returns nothing
- WarVKSetWeatherPaused(integer id, boolean paused) returns nothing
- WarVKSetWeatherTime(integer id, real seconds) returns nothing
- WarVKGetWeatherTime(integer id) returns real

weather.* 命令沿 warvk:v1，create/isAlive 走 Hotkey，getTime 走 LocalizedString，写操作走 Preloader。
runtime 提供的内部 id 可以直接返回；若 API lane 采用 ManagedType 包装须明确映射并在 reset 中清空。
seed 允许有符号 JASS integer 的完整 bit pattern。参数有限性与范围在 runtime 再验证。
强度五项 snowfall/gust/haze/lowSnow 0..1（windAngle 除外），windSpeed 0..2048 世界单位/秒，
windAngle 允许有限角度并规范化，transitionSeconds 0..3600，时间 0..86400，位置绝对值 <= 1e7，
严格 maxX>minX/maxY>minY/topZ>bottomZ，edgeFeather 0..0.5。
create 初始关闭；Play 自动开启；SetEnabled(false) 冻结时间、配置保留；恢复继续。
SetPaused 保持当前显示与时间，禁止暂停期间新墙钟积分。
SetTarget 从当前求值开始切手动，原时间线停止；时间线最后保持。
关键帧首项必须 0，后续严格递增；仅停止时间线时可修改。windAngle 最短方向插值。
风位移连续积分，换风向/速度不能把历史雪片瞬间传送。Seek 明确重建天气历史。
没有天气实例时不分配 GPU 天气资源、无额外 geometry capture、无 shader scan。

## GPU 选择

首版优先用 GPU vertex 实例化求值：每个雪片的 hash/生命周期/世界位置由 seed、天气时间、
连续风位移与有界相机区域计算。这样无需 CPU 每粒子更新、GPU ping-pong 写回或大地图快照。
这是相同 GPU 运动目标的简化实现；如该方案不能提供连续可信运动，render lane 先报告证据再调整。
固定最大粒子数，世界空间运动，premultiplied alpha，深度遮挡/边缘软化；不得逐片投影。
粒子不进入 shadowCasters/Stage11/Arena。风雪不修改 caster 完整性或既有预算。
root 添加独立 Froxel 天气介质；普通 fog 配置保持由原 API 所有者控制。
GPU resource 跟踪/retirement 使用 DXVK 既有机制，当前命令已追踪资源才允许失效或替换。
不得复制 SSAO 当前 in-flight depth-copy 隐患；所需深度优先直接只读 input.depthView，带精确合法布局恢复。

## 测试与完成

各 lane 先提交源码与测例，未获 lease 不编译。主线程审查后统一构建。
必须包含：无效/过期 id、NaN/Inf、容量、范围、时间线边界、暂停/恢复、disable 时间、seek、
最短角度、风位移连续、reset、并发按值读取、JASS→生产 Dispatch→生产快照、Shader ABI。
实机证据与 CPU/build 区分。最终 DLL 保存 size/SHA 与配方，未验收画面不标稳定。
