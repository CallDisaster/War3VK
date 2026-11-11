# 可编排风雪 API（2026-10-10 开发候选）

这组接口让地图作者编排“小雪 → 大雪 → 暴风雪”，而不是更改单位行为或另造一套音频系统。
本页描述当前源码合同，不代表已经发布或所有实机门都已通过；验收以主线程最新 checkpoint 和对应 DLL 身份为准。

默认没有天气实例，**未创建并启用实例时，不会为天气效果创建 GPU 资源或绘制天气**。
创建本身只配置 CPU 状态，实例初始关闭。公开 API 不需要额外的天气环境变量；
开发诊断的开关不等于作者功能的启用条件。曾启用过的资源仍按现有 GPU 资源寿命处理，不能把停用理解成显存瞬时全部归零。

## 本轮能做什么

室外区域中的雪片、独立体积雪雾和低空吹雪使用同一组作者风场参数，可以手动渐变，也可以按关键帧演出。
v1 **每个地图会话最多一个天气实例，每个实例最多 64 个关键帧**，不提供多天气混合。
换图会清除作者实例，旧地图的天气句柄不能操作新地图实例。

音乐继续使用 Warcraft III 原版音频函数。作者应试听**实际使用的录音**，手动标记前奏、发展段和高潮的秒数，
再在同一触发器中启动音乐及天气时间线。系统**不会自动分析音乐、识别高潮、追踪节拍或读取音频播放位置**，
也不承诺与音频设备时钟采样级同步。不同指挥、速度和录音的乐句时间不能直接套用。

## 12 个 API：完整签名与分类

以下是 JASS 类型：`integer` 为整数，`real` 为实数，`boolean` 为布尔值，`nothing` 为无返回值。
`weatherId` 是创建函数返回的受管整数句柄，不是原版 `weathereffect`；不要与其他 WarVK 对象句柄混用。

| 分类 | 函数与参数类型 | 返回类型 | 作用 |
| --- | --- | --- | --- |
| 天气 / 生命周期 | `WarVKCreateSnowWeather(integer seed)` | `integer` | 创建初始关闭的实例；成功返回正整数，失败返回 0 |
| 天气 / 生命周期 | `WarVKDestroyWeather(integer weatherId)` | `nothing` | 销毁实例，使旧句柄失效 |
| 天气 / 查询 | `WarVKIsWeatherAlive(integer weatherId)` | `boolean` | 查询句柄是否仍有效；失效的有效格式句柄返回 false |
| 天气 / 区域 | `WarVKSetWeatherRegion(integer weatherId, real minX, real minY, real maxX, real maxY, real bottomZ, real topZ, real edgeFeather)` | `nothing` | 设置世界 XY 矩形、高度及边缘柔化 |
| 天气 / 开关 | `WarVKSetWeatherEnabled(integer weatherId, boolean enabled)` | `nothing` | 隐藏/恢复天气；停用冻结时钟并保留配置，不解除已有暂停状态 |
| 天气 / 手动演出 | `WarVKSetSnowWeatherTarget(integer weatherId, real snowfall, real windAngle, real windSpeed, real gust, real haze, real lowSnow, real transitionSeconds)` | `nothing` | 从当前外观切到手动模式并过渡到目标；不会自动启用实例 |
| 天气 / 时间线编辑 | `WarVKClearWeatherKeyframes(integer weatherId)` | `nothing` | 清空关键帧；运行或暂停着的时间线不能直接编辑 |
| 天气 / 时间线编辑 | `WarVKAddSnowWeatherKeyframe(integer weatherId, real timeSeconds, real snowfall, real windAngle, real windSpeed, real gust, real haze, real lowSnow)` | `nothing` | 加入一个关键帧，首帧 0 秒，其后时间严格递增 |
| 天气 / 时间线播放 | `WarVKPlayWeatherTimeline(integer weatherId, real startSeconds)` | `nothing` | 从指定时刻播放，自动启用并解除暂停；至少须有一个关键帧 |
| 天气 / 时间控制 | `WarVKSetWeatherPaused(integer weatherId, boolean paused)` | `nothing` | 冻结/恢复运动、演出时间和正在进行的渐变；暂停仍保留显示 |
| 天气 / 时间控制 | `WarVKSetWeatherTime(integer weatherId, real seconds)` | `nothing` | 跳转演出时刻，重建天气历史；不自动启用或解除暂停 |
| 天气 / 查询 | `WarVKGetWeatherTime(integer weatherId)` | `real` | 返回天气演出秒数；失败返回 0，须结合错误码判断 |

本树的 YDWE `action.txt` / `call.txt` / `define.txt` 已登记这些函数；使用旧安装目录不会自动出现新条目。
天气接口不会替代原版天气、声音函数，也不会改变移动、伤害、寻路或防守波次；那些仍由地图触发器控制。

## 参数单位和范围

| 参数 | JASS 类型 | 范围 / 单位 | 作者应如何理解 |
| --- | --- | --- | --- |
| `seed` | `integer` | 完整有符号整数位模式 | 控制确定的程序化空间分布；不是粒子数量 |
| `snowfall` | `real` | 0..1 | 雪片强度；0 不记录雪片绘制，但 haze / lowSnow 仍可独立运行 |
| `windAngle` | `real` | 有限角度，单位度 | 0 沿世界 +X，90 沿 +Y；角度归一化，过渡取最短方向 |
| `windSpeed` | `real` | 0..2048，世界单位/秒 | 累计风位移；不是“当前速度 × 总时间” |
| `gust` | `real` | 0..1 | 雪片摆动和介质条带调制，不是游戏单位所受风力 |
| `haze` | `real` | 0..1 | 独立体积雪雾，改变视线上的散射与透射 |
| `lowSnow` | `real` | 0..1 | 靠近作者 `bottomZ` 的低空介质；不等于贴地粒子或地面积雪 |
| `transitionSeconds` | `real` | 0..3600 秒 | 手动过渡时长；0 表示立即到目标 |
| `timeSeconds` / `startSeconds` / `seconds` | `real` | 0..86400 秒 | 天气演出时刻；不是毫秒、游戏业务帧或音乐播放位置 |
| `minX,minY,maxX,maxY,bottomZ,topZ` | `real` | 世界单位，各项绝对值 ≤1e7 | Z 向上，要求 maxX>minX、maxY>minY、topZ>bottomZ |
| `edgeFeather` | `real` | 0..0.5 | 按区域跨度计算的边缘柔化比例 |

所有数值必须有限。NaN、Infinity、越界、错误参数类型、错误调用载体和失效句柄都不能用来更新天气。
不要通过传超范围强度来扩大资源预算。`haze=0.8` 与 `lowSnow=0.8` 并不承诺相同的全屏白度：
低雪集中在底部附近，正常俯视时它的光学影响可能显著小于全高度雪雾。

JASS 包装函数使用原版 `R2S` 序列化 real 参数，不应依赖亚毫秒输入或超出该文本表示的精度。
常规演出使用整秒或毫秒级参数即可；查询返回 `real` 也不是一个用于音频硬同步的高精度时钟协议。

## 开关、暂停、跳转：不要混为一件事

天气演出使用单调实时时钟推进；**只在实例 enabled=true 且 paused=false 时推进**，
最大演出时刻为 86400 秒。它不会替作者同步音乐暂停，也不能把游戏暂停或原版声音暂停视为天气暂停 API 的替代品。

| 操作 | 显示 | 时间 / 外观 | 重要区别 |
| --- | --- | --- | --- |
| `SetWeatherEnabled(id,false)` | 隐藏天气 | 冻结时钟，保留参数、关键帧及暂停标志 | 再设 true 从原时刻续接；如果仍 paused，就不会运动 |
| `SetWeatherPaused(id,true)` | 已启用的天气继续显示 | 时间、风位移和渐变冻结 | 不隐藏天气；设 false 也不会把 disabled 变成 enabled |
| `PlayWeatherTimeline(id,t)` | 启用天气 | 进入时间线模式，定位到 t 并开始推进 | 会解除暂停；超过末关键帧后保持末帧外观，不自动停止或循环 |
| `SetWeatherTime(id,t)` | 不改变开关 | 时间线模式按关键帧重新求值；手动模式保持当前外观并结束未完过渡 | 重置累计风位移、使天气历史失效；不倒放过去的粒子轨迹，也不恢复过去画面 |
| `SetSnowWeatherTarget(id,...,duration)` | 不改变开关 | 退出时间线，按当前演出时刻开始手动过渡 | 过渡在停用或暂停时也停止计时；duration=0 立即变更 |

关键帧之间是参数线性插值，风向使用最短弧；需要一段稳定小雪或稳定暴雪，就在该段起止各放一个相同参数的关键帧。
末帧外观保持时，天气时钟和运动仍可继续，直到暂停、停用或达到时间上限。

### 修改已经播放的时间线

新创建、尚未播放的实例可直接添加关键帧。已经进入时间线模式后：

- enabled=true 时不能直接清空或添加，即使已经 paused=true。
- disabled 但 paused=true 的时间线也不能编辑。
- 可先停用并设 paused=false，再编辑；第一次成功编辑会退出原时间线、保持当前外观。失败编辑不切换模式、不修改原关键帧。
- 另一种做法是先用 `SetSnowWeatherTarget` 明确切回手动模式，再编辑；手动模式的 paused 状态本身不禁止编辑。

例如，在有效句柄上逐条调用并检查错误：

```jass
call WarVKSetWeatherEnabled(weatherId, false)
call WarVKSetWeatherPaused(weatherId, false)
call WarVKClearWeatherKeyframes(weatherId)
// 接着重新添加：首帧仍必须从 0.0 秒开始。
```

## 错误读取：紧跟操作，不只看返回值

天气写操作返回 `nothing`，并不表示一定成功。沿用两个已有查询接口，不新增天气专用错误 API：

```jass
// WarVKGetLastErrorCode takes nothing returns integer
// WarVKGetLastError takes nothing returns string
call WarVKSetWeatherEnabled(weatherId, true)
if WarVKGetLastErrorCode() != WARVK_ERROR_NONE then
    call BJDebugMsg("Weather: " + WarVKGetLastError())
endif
```

应在下一个非错误查询的 WarVK 操作前读取错误，因为后续成功操作可能覆盖失败信息。
需要清理实例时，先保存或打印原错误，再执行 Destroy；否则清理成功可能把最初失败原因覆盖。
`GetWeatherTime` 返回 0 既可能是合法起点，也可能是失败，必须看错误码。
`IsWeatherAlive` 对已销毁的正整数句柄正常返回 false，不能单凭它把“失效”当作解析错误；非法句柄格式仍可能报错。

很多作者语义错误会统一返回 `WARVK_ERROR_BACKEND_REJECTED`，例如第二个实例、关键帧超限、时间不递增、
运行中的时间线编辑、越界参数或无法解析为当前天气对象的句柄。不要假定每个业务原因都有不同错误码。
协议层的错误类型、数量、载体或非法 real 则沿用既有更具体的协议错误。

## 可直接参考的三段演出：小雪 → 大雪 → 暴雪

下列时刻是**说明用的 0/30/60/90/120 秒**，不是柴可夫斯基第六交响曲第三乐章的真实标记。
将时间替换成实际录音的乐句位置。各阶段只改变画面，防守波次由自己的触发器安排。

| 音乐 / 演出段 | 时间示例 | 雪片 / 风 / 介质设计 |
| --- | --- | --- |
| 小雪：建立冷空气 | 0–30 秒 | 稀疏雪片、慢风，两个相同关键帧让开头保持稳定；暂不加低雪 |
| 大雪：发展段变得不安 | 30–60 秒 | 增加雪片、横风和条带；从小雪连续过渡到大雪 |
| 暴雪：高潮但仍可操作 | 60–90 秒渐强，90–120 秒保持 | 雪片和阵风增强，雪雾适度上升，低雪加强；不要遮掉敌人轮廓和防守区域 |

先将当前 `warvk_constant.j`、`warvk_api.j` 加入地图，再把以下代码放入自定义 JASS，
由音乐开始的同一个触发器调用 `WinterWeatherStart()`。区域底高 0.0 仅适合地面接近 Z=0 的示例区域，
实际地图必须自己选择适当高度。

```jass
globals
    integer winterWeatherId = 0
endglobals

function WinterWeatherCheck takes nothing returns boolean
    if WarVKGetLastErrorCode() == WARVK_ERROR_NONE then
        return true
    endif
    // 先报告原错误，再清理；不能让 Destroy 覆盖最初原因。
    call BJDebugMsg("Weather setup failed: " + WarVKGetLastError())
    call WarVKDestroyWeather(winterWeatherId)
    set winterWeatherId = 0
    return false
endfunction

function WinterWeatherStart takes nothing returns nothing
    if not WarVKIsBridgeAvailable() then
        call BJDebugMsg("WarVK bridge unavailable")
        return
    endif
    if winterWeatherId > 0 and WarVKIsWeatherAlive(winterWeatherId) then
        call WarVKDestroyWeather(winterWeatherId)
    endif
    set winterWeatherId = WarVKCreateSnowWeather(20261225)
    if winterWeatherId <= 0 then
        call BJDebugMsg("Weather creation failed: " + WarVKGetLastError())
        return
    endif
    call WarVKSetWeatherRegion(winterWeatherId, -4096.0, -4096.0, 4096.0, 4096.0, 0.0, 1800.0, 0.15)
    if not WinterWeatherCheck() then
        return
    endif
    // 次序：id, 秒数, snowfall, 角度, 风速, gust, haze, lowSnow。
    // 第一段：小雪；0 和 30 秒同参数，保持而不是从一开始就渐强。
    call WarVKAddSnowWeatherKeyframe(winterWeatherId, 0.0, 0.10, 20.0, 40.0, 0.10, 0.05, 0.00)
    if not WinterWeatherCheck() then
        return
    endif
    call WarVKAddSnowWeatherKeyframe(winterWeatherId, 30.0, 0.10, 20.0, 40.0, 0.10, 0.05, 0.00)
    if not WinterWeatherCheck() then
        return
    endif
    // 第二段：30 到 60 秒连续过渡到大雪。
    call WarVKAddSnowWeatherKeyframe(winterWeatherId, 60.0, 0.40, 30.0, 160.0, 0.35, 0.25, 0.20)
    if not WinterWeatherCheck() then
        return
    endif
    // 第三段：60 到 90 秒过渡到暴雪，90 到 120 秒保持。
    call WarVKAddSnowWeatherKeyframe(winterWeatherId, 90.0, 0.90, 40.0, 420.0, 0.80, 0.65, 0.75)
    if not WinterWeatherCheck() then
        return
    endif
    call WarVKAddSnowWeatherKeyframe(winterWeatherId, 120.0, 0.90, 40.0, 420.0, 0.80, 0.65, 0.75)
    if not WinterWeatherCheck() then
        return
    endif
    call WarVKPlayWeatherTimeline(winterWeatherId, 0.0)
    if not WinterWeatherCheck() then
        return
    endif
endfunction
```

这段只用 5 个关键帧；120 秒后仍保持暴雪，**不会自动停止**。结束防守活动时，
可以调用 Destroy 并把自己的句柄变量设 0；想保留配置供下一次活动重播，则先停用，再在下次调用 PlayTimeline(id,0.0)。
已有独立参考文件 [warvk_weather_demo.j](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/WarVK/jass/warvk_weather_demo.j)
采用 0/30/60/90 秒的四帧设计，90 秒回落到较轻状态；与本页“高潮保持”的五帧例子不要混为一个时间表。

### 音乐控制的配套调用

这些语句由作者的音乐暂停/继续/定位触发器执行；每次仍须马上读取错误：

```jass
// 同时暂停自己的音乐，并冻结天气；仍保留当前雪景。
call WarVKSetWeatherPaused(winterWeatherId, true)
// 音乐继续时恢复天气；不会重新开始时间线。
call WarVKSetWeatherPaused(winterWeatherId, false)
// 作者把音乐定位到 60 秒后，将天气时间也定位到 60 秒。
// 保留原 enabled / paused 状态；不是重播 PlayTimeline。
call WarVKSetWeatherTime(winterWeatherId, 60.0)
```

上面是三个**分别对应不同事件**的调用示意，不是让同一触发器紧接着执行暂停、继续、跳转。
不要每帧按音乐估计位置反复调用 SetTime；每次 seek 会重置天气历史与累计风位移，造成不必要的不连续。

## 技术、艺术与验收边界

雪片最多 16,384 个，采用固定容量 GPU 实例化求值，不逐粒子上传，也不进入阴影 caster、Stage11 快照页或 Arena。
它围绕相机选择有界世界区域，不为走遍全地图缓存永久雪片。雪雾复用体积介质代码，但有独立资源和设置所有者，
不改作者已有的全局雾、太阳或点光参数；不要求通过开启普通体积光或太阳来开启天气雪雾。

区域是**作者填写的平面底高和顶部高度，不会自动贴合地形**。低雪靠近 bottomZ，而不是靠近每一处地面；
地形高低落差很大时，单一底高不能等同于整个地图的贴地吹雪。室外区域也不会自动识别建筑、室内或屋顶。
v1 不包含屋顶遮雪、建筑随机积雪、积雪网格、脚印、精确地形跟随、战争迷雾过滤或单位局部天气照明。
“lowSnow”是薄层介质名称，不是已经完成这些功能的承诺。

建议先把雪片、haze 和 lowSnow 分别检查，再检查组合，不要只凭“全屏变白”判断三部分都在工作。
小雪应可辨认运动方向；大雪增加压力但保留单位轮廓；暴雪允许更强冷色与横风，却不能遮掉玩家的主要操作信息。
关键帧参数是作者美术起点，不是已经跨地图、跨 GPU 调校完毕的统一效果预设。

Render Stats 与 `weather-status` 区分作者实时状态、实际消费的冻结快照、雪片命令及雪雾合成命令。
命令已记录不等于 GPU 已完成，更不等于视觉验收；当前资源字节不是包含 Driver 和在途退役资源的显存峰值。
单个旧 canary 或压力测试中的局部通过不等于当前候选全门通过。本页不据此宣布稳定或发布。
首版只接受单采样目标；非常远的镜头、分辨率切换、复杂透明物体与玩家实际防守场景仍须按对应候选身份验证。
