globals
    // 私有数值通道。桥接不可用时所有公开函数自动回退到 warvk:v1 字符串协议。
    hashtable wvkTypedTable = null
    integer wvkTypedState = 0
    integer wvkTypedSequence = 0

    constant integer wvkTypedRegisterParent = 1465273172
    constant integer wvkTypedRegisterChildA = 1380271921
    constant integer wvkTypedRegisterChildB = 1380271922
    constant integer wvkTypedRegisterCookieA = 324478056
    constant integer wvkTypedRegisterCookieB = 610800471
    constant integer wvkTypedProbeChild = 1347571522
    constant integer wvkTypedProbeAck = 1464555058
    constant integer wvkTypedBeginChild = -2147418111
    constant integer wvkTypedCommitChild = -2147418110
    constant integer wvkTypedQueryIntegerChild = -2147418109
    constant integer wvkTypedQueryRealChild = -2147418108

    constant integer wvkTypedPointLightSetPosition = 101
    constant integer wvkTypedPointLightSetColorIntensity = 102
    constant integer wvkTypedPointLightSetRadius = 103
    constant integer wvkTypedMathEvaluateReal = 201
    constant integer wvkTypedMathEvaluateInteger = 202
    constant integer wvkTypedCurveEvaluateComponent = 203
    constant integer wvkTypedCurveDerivativeComponent = 204
    constant integer wvkTypedCurveArcLength = 205
    constant integer wvkTypedCurvePointAppend4 = 206
    constant integer wvkTypedLightningSetEndpoints = 301
    constant integer wvkTypedLightningSetColor = 302
    constant integer wvkTypedLightningSetWidth = 303
    constant integer wvkTypedTimeVisualSeconds = 401
    constant integer wvkTypedStatsFramesPerSecond = 402
    constant integer wvkTypedStatsFrameTimeMilliseconds = 403

endglobals

function WVKTypedReady takes nothing returns boolean
    if wvkTypedState == 1 then
        return true
    endif
    if wvkTypedState == -1 then
        return false
    endif
    set wvkTypedTable = InitHashtable()
    if wvkTypedTable == null then
        set wvkTypedState = -1
        return false
    endif
    call SaveInteger(wvkTypedTable, wvkTypedRegisterParent, wvkTypedRegisterChildA, wvkTypedRegisterCookieA)
    call SaveInteger(wvkTypedTable, wvkTypedRegisterParent, wvkTypedRegisterChildB, wvkTypedRegisterCookieB)
    if LoadInteger(wvkTypedTable, wvkTypedRegisterParent, wvkTypedProbeChild) == wvkTypedProbeAck then
        set wvkTypedState = 1
        return true
    endif
    set wvkTypedState = -1
    return false
endfunction

function WVKTypedBegin takes integer opcode returns integer
    if wvkTypedSequence <= 0 or wvkTypedSequence >= 2147483000 then
        set wvkTypedSequence = 1
    else
        set wvkTypedSequence = wvkTypedSequence + 1
    endif
    call SaveInteger(wvkTypedTable, wvkTypedSequence, wvkTypedBeginChild, opcode)
    return wvkTypedSequence
endfunction

function WarVKBoolToken takes boolean value returns string
    if value then
        return "1"
    endif
    return "0"
endfunction

function WarVKGetVersion takes nothing returns string
    local string payload = "warvk:v1;system.version"
    return GetLocalizedString(payload)
endfunction

function WarVKGetProtocolVersion takes nothing returns integer
    local string payload = "warvk:v1;system.protocolVersion"
    return GetLocalizedHotkey(payload)
endfunction

// 仅表示内置 warvk:v1 桥接已接受协议。
// 不要求渲染管线已就绪：地图初始化阶段即可注册 CPU 侧闪电模板。
function WarVKIsBridgeAvailable takes nothing returns boolean
    return WarVKGetProtocolVersion() == 1
endfunction

function WarVKGetLastErrorCode takes nothing returns integer
    local string payload = "warvk:v1;system.lastErrorCode"
    return GetLocalizedHotkey(payload)
endfunction

function WarVKGetLastError takes nothing returns string
    local string payload = "warvk:v1;system.lastError"
    return GetLocalizedString(payload)
endfunction

function WarVKClearError takes nothing returns nothing
    local string payload = "warvk:v1;system.clearError"
    call Preloader(payload)
endfunction

function WarVKGetFeatureFlags takes nothing returns integer
    local string payload = "warvk:v1;system.featureFlags"
    return GetLocalizedHotkey(payload)
endfunction

function WarVKIsRuntimeReady takes nothing returns boolean
    local string payload = "warvk:v1;system.runtimeReady"
    return GetLocalizedHotkey(payload) != 0
endfunction

function WarVKSetSunEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;sun.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetSunDirection takes real x, real y, real z returns nothing
    local string payload = "warvk:v1;sun.setDirection" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z)
    call Preloader(payload)
endfunction

function WarVKSetSunColorIntensity takes real red, real green, real blue, real intensity returns nothing
    local string payload = "warvk:v1;sun.setColorIntensity" + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(intensity)
    call Preloader(payload)
endfunction

function WarVKSetCsmEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;csm.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetCsmLayout takes integer cascadeCount, real maxDistance returns nothing
    local string payload = "warvk:v1;csm.setLayout" + ";i:" + I2S(cascadeCount) + ";r:" + R2S(maxDistance)
    call Preloader(payload)
endfunction

function WarVKSetCsmTuning takes real depthBias, real blendWidth returns nothing
    local string payload = "warvk:v1;csm.setTuning" + ";r:" + R2S(depthBias) + ";r:" + R2S(blendWidth)
    call Preloader(payload)
endfunction

function WarVKCreatePointLight takes real x, real y, real z, real radius, real red, real green, real blue, real intensity returns integer
    local string payload = "warvk:v1;pointLight.create" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z) + ";r:" + R2S(radius) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(intensity)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKDestroyPointLight takes integer lightId returns nothing
    local string payload = "warvk:v1;pointLight.destroy" + ";d:" + I2S(lightId)
    call Preloader(payload)
endfunction

function WarVKSetPointLightEnabled takes integer lightId, boolean enabled returns nothing
    local string payload = "warvk:v1;pointLight.setEnabled" + ";d:" + I2S(lightId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetPointLightPosition takes integer lightId, real x, real y, real z returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedPointLightSetPosition)
        call SaveInteger(wvkTypedTable, transaction, 0, lightId)
        call SaveReal(wvkTypedTable, transaction, 1, x)
        call SaveReal(wvkTypedTable, transaction, 2, y)
        call SaveReal(wvkTypedTable, transaction, 3, z)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedPointLightSetPosition)
        return
    endif
    set payload = "warvk:v1;pointLight.setPosition" + ";d:" + I2S(lightId) + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z)
    call Preloader(payload)
endfunction

function WarVKSetPointLightColorIntensity takes integer lightId, real red, real green, real blue, real intensity returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedPointLightSetColorIntensity)
        call SaveInteger(wvkTypedTable, transaction, 0, lightId)
        call SaveReal(wvkTypedTable, transaction, 1, red)
        call SaveReal(wvkTypedTable, transaction, 2, green)
        call SaveReal(wvkTypedTable, transaction, 3, blue)
        call SaveReal(wvkTypedTable, transaction, 4, intensity)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedPointLightSetColorIntensity)
        return
    endif
    set payload = "warvk:v1;pointLight.setColorIntensity" + ";d:" + I2S(lightId) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(intensity)
    call Preloader(payload)
endfunction

function WarVKSetPointLightRadius takes integer lightId, real radius returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedPointLightSetRadius)
        call SaveInteger(wvkTypedTable, transaction, 0, lightId)
        call SaveReal(wvkTypedTable, transaction, 1, radius)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedPointLightSetRadius)
        return
    endif
    set payload = "warvk:v1;pointLight.setRadius" + ";d:" + I2S(lightId) + ";r:" + R2S(radius)
    call Preloader(payload)
endfunction

function WarVKSetPointLightShadowEnabled takes integer lightId, boolean enabled returns nothing
    local string payload = "warvk:v1;pointLight.setShadowEnabled" + ";d:" + I2S(lightId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetPointLightShadowConfig takes integer lightId, integer resolution, real bias returns nothing
    local string payload = "warvk:v1;pointLight.setShadowConfig" + ";d:" + I2S(lightId) + ";i:" + I2S(resolution) + ";r:" + R2S(bias)
    call Preloader(payload)
endfunction

function WarVKIsPointLightAlive takes integer lightId returns boolean
    local string payload = "warvk:v1;pointLight.isAlive" + ";d:" + I2S(lightId)
    return GetLocalizedHotkey(payload) != 0
endfunction

function WarVKSetVolumetricEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;volumetric.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 只控制均匀的全局介质；false 保留局部 Sphere/Box/Cylinder 及其光照和阴影。
// 该开关不会修改 density，重新启用时会恢复之前设置的全局密度。
function WarVKSetGlobalVolumetricMediumEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;volumetric.setGlobalMediumEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetVolumetricDensity takes real density returns nothing
    local string payload = "warvk:v1;volumetric.setDensity" + ";r:" + R2S(density)
    call Preloader(payload)
endfunction

function WarVKSetVolumetricScattering takes real scattering, real decay returns nothing
    local string payload = "warvk:v1;volumetric.setScattering" + ";r:" + R2S(scattering) + ";r:" + R2S(decay)
    call Preloader(payload)
endfunction

function WarVKSetVolumetricQuality takes integer stepCount, real maxDistance returns nothing
    local string payload = "warvk:v1;volumetric.setQuality" + ";i:" + I2S(stepCount) + ";r:" + R2S(maxDistance)
    call Preloader(payload)
endfunction

// 0=旧 RayMarch，1=Froxel Medium，2=Froxel High。Froxel 仍需单独开启体积光。
function WarVKSetVolumetricBackend takes integer backend returns nothing
    local string payload = "warvk:v1;volumetric.setBackend" + ";i:" + I2S(backend)
    call Preloader(payload)
endfunction

// 高度雾开关不会自动开启体积光通道；要看到雾效还需调用
// WarVKSetVolumetricEnabled(true)。关闭高度雾不会关闭太阳/点光散射。
function WarVKSetGlobalVolumetricFogEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;volumetricFog.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// baseHeight 为雾层基准世界高度；falloff 为随高度衰减率（0..0.05）；
// strength 为全局高度雾强度（0..2）。
function WarVKSetGlobalVolumetricFog takes real baseHeight, real falloff, real strength returns nothing
    local string payload = "warvk:v1;volumetricFog.setSettings" + ";r:" + R2S(baseHeight) + ";r:" + R2S(falloff) + ";r:" + R2S(strength)
    call Preloader(payload)
endfunction

// 局部雾使用世界坐标。Box 的三个 size 与 Cylinder 的 height 都是完整尺寸；
// edgeFeather 为 0..1 的归一化边缘过渡宽度。创建局部雾不会自动开启体积光通道。
function WarVKCreateSphereFogVolume takes real x, real y, real z, real radius, real density, real edgeFeather returns integer
    local string payload = "warvk:v1;localFog.createSphere" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z) + ";r:" + R2S(radius) + ";r:" + R2S(density) + ";r:" + R2S(edgeFeather)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKCreateBoxFogVolume takes real x, real y, real z, real sizeX, real sizeY, real sizeZ, real density, real edgeFeather returns integer
    local string payload = "warvk:v1;localFog.createBox" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z) + ";r:" + R2S(sizeX) + ";r:" + R2S(sizeY) + ";r:" + R2S(sizeZ) + ";r:" + R2S(density) + ";r:" + R2S(edgeFeather)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKCreateCylinderFogVolume takes real x, real y, real z, real radius, real height, real density, real edgeFeather returns integer
    local string payload = "warvk:v1;localFog.createCylinder" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z) + ";r:" + R2S(radius) + ";r:" + R2S(height) + ";r:" + R2S(density) + ";r:" + R2S(edgeFeather)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKDestroyFogVolume takes integer fogId returns nothing
    local string payload = "warvk:v1;localFog.destroy" + ";d:" + I2S(fogId)
    call Preloader(payload)
endfunction

function WarVKSetFogVolumeEnabled takes integer fogId, boolean enabled returns nothing
    local string payload = "warvk:v1;localFog.setEnabled" + ";d:" + I2S(fogId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetFogVolumePosition takes integer fogId, real x, real y, real z returns nothing
    local string payload = "warvk:v1;localFog.setPosition" + ";d:" + I2S(fogId) + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z)
    call Preloader(payload)
endfunction

function WarVKSetFogVolumeRotation takes integer fogId, real xDegrees, real yDegrees, real zDegrees returns nothing
    local string payload = "warvk:v1;localFog.setRotation" + ";d:" + I2S(fogId) + ";r:" + R2S(xDegrees) + ";r:" + R2S(yDegrees) + ";r:" + R2S(zDegrees)
    call Preloader(payload)
endfunction

function WarVKSetFogVolumeDensity takes integer fogId, real density returns nothing
    local string payload = "warvk:v1;localFog.setDensity" + ";d:" + I2S(fogId) + ";r:" + R2S(density)
    call Preloader(payload)
endfunction

function WarVKSetFogVolumeEdgeFeather takes integer fogId, real edgeFeather returns nothing
    local string payload = "warvk:v1;localFog.setEdgeFeather" + ";d:" + I2S(fogId) + ";r:" + R2S(edgeFeather)
    call Preloader(payload)
endfunction

function WarVKSetSphereFogVolumeRadius takes integer fogId, real radius returns nothing
    local string payload = "warvk:v1;localFog.setSphereRadius" + ";d:" + I2S(fogId) + ";r:" + R2S(radius)
    call Preloader(payload)
endfunction

function WarVKSetBoxFogVolumeSize takes integer fogId, real sizeX, real sizeY, real sizeZ returns nothing
    local string payload = "warvk:v1;localFog.setBoxSize" + ";d:" + I2S(fogId) + ";r:" + R2S(sizeX) + ";r:" + R2S(sizeY) + ";r:" + R2S(sizeZ)
    call Preloader(payload)
endfunction

function WarVKSetCylinderFogVolumeSize takes integer fogId, real radius, real height returns nothing
    local string payload = "warvk:v1;localFog.setCylinderSize" + ";d:" + I2S(fogId) + ";r:" + R2S(radius) + ";r:" + R2S(height)
    call Preloader(payload)
endfunction

function WarVKIsFogVolumeAlive takes integer fogId returns boolean
    local string payload = "warvk:v1;localFog.isAlive" + ";d:" + I2S(fogId)
    return GetLocalizedHotkey(payload) != 0
endfunction

function WarVKSetOutlineEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;outline.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetOutlineColor takes real red, real green, real blue, real alpha returns nothing
    local string payload = "warvk:v1;outline.setColor" + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(alpha)
    call Preloader(payload)
endfunction

function WarVKSetOutlineParameters takes real width, real depthThreshold returns nothing
    local string payload = "warvk:v1;outline.setParameters" + ";r:" + R2S(width) + ";r:" + R2S(depthThreshold)
    call Preloader(payload)
endfunction

function WarVKSetBloomEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;bloom.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetBloomParameters takes real threshold, real intensity, real softKnee returns nothing
    local string payload = "warvk:v1;bloom.setParameters" + ";r:" + R2S(threshold) + ";r:" + R2S(intensity) + ";r:" + R2S(softKnee)
    call Preloader(payload)
endfunction

function WarVKSetBloomRadius takes real radius returns nothing
    local string payload = "warvk:v1;bloom.setRadius" + ";r:" + R2S(radius)
    call Preloader(payload)
endfunction

function WarVKSetPostfxEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;postfx.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetPostfxExposureGamma takes real exposure, real gamma returns nothing
    local string payload = "warvk:v1;postfx.setExposureGamma" + ";r:" + R2S(exposure) + ";r:" + R2S(gamma)
    call Preloader(payload)
endfunction

function WarVKSetPostfxColorGrade takes real saturation, real contrast returns nothing
    local string payload = "warvk:v1;postfx.setColorGrade" + ";r:" + R2S(saturation) + ";r:" + R2S(contrast)
    call Preloader(payload)
endfunction

function WarVKSetAaMode takes integer mode, integer quality returns nothing
    local string payload = "warvk:v1;aa.setMode" + ";i:" + I2S(mode) + ";i:" + I2S(quality)
    call Preloader(payload)
endfunction

function WarVKSetAaSharpness takes real sharpness returns nothing
    local string payload = "warvk:v1;aa.setSharpness" + ";r:" + R2S(sharpness)
    call Preloader(payload)
endfunction

function WarVKSetDayNightEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;dayNight.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetDayNightTime takes real hours returns nothing
    local string payload = "warvk:v1;dayNight.setTime" + ";r:" + R2S(hours)
    call Preloader(payload)
endfunction

function WarVKSetDayNightSpeed takes real scale returns nothing
    local string payload = "warvk:v1;dayNight.setSpeed" + ";r:" + R2S(scale)
    call Preloader(payload)
endfunction

// 选择 WarVK 光照时钟来源：跟随游戏、保持当前时刻或独立推进。
// 该设置只影响渲染，不改变 Warcraft 的玩法时间。
function WarVKSetLightingClockMode takes integer mode returns nothing
    local string payload = "warvk:v1;lightingClock.setMode" + ";i:" + I2S(mode)
    call Preloader(payload)
endfunction

// 设置 0..24 小时的当前渲染时刻，并自动切换到 HELD 模式。
function WarVKSetLightingClockTime takes real hours returns nothing
    local string payload = "warvk:v1;lightingClock.holdTime" + ";r:" + R2S(hours)
    call Preloader(payload)
endfunction

// 设置独立光照时钟走完 24 小时所需的现实秒数（1..86400）。
function WarVKSetLightingDayDuration takes real seconds returns nothing
    local string payload = "warvk:v1;lightingClock.setDayDuration" + ";r:" + R2S(seconds)
    call Preloader(payload)
endfunction

// 控制太阳/月亮方向及方向光阴影强度是否随光照时钟变化。
function WarVKSetCelestialMotionEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;lightingCycle.setCelestialMotionEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 控制方向光色温、亮度和环境色调是否随光照时钟变化。
function WarVKSetTimeColorGradingEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;lightingCycle.setTimeColorGradingEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 设置 00:00、06:00、12:00、18:00 四个采样点的色温（1000..20000K）。
function WarVKSetTimeColorTemperatureProfile takes real midnightKelvin, real dawnKelvin, real noonKelvin, real duskKelvin returns nothing
    local string payload = "warvk:v1;lightingCycle.setColorTemperatureProfile" + ";r:" + R2S(midnightKelvin) + ";r:" + R2S(dawnKelvin) + ";r:" + R2S(noonKelvin) + ";r:" + R2S(duskKelvin)
    call Preloader(payload)
endfunction

// 恢复 WarVK 内置的自然昼夜色温曲线。
function WarVKResetTimeColorTemperatureProfile takes nothing returns nothing
    local string payload = "warvk:v1;lightingCycle.resetColorTemperatureProfile"
    call Preloader(payload)
endfunction

function WarVKCreateLightning takes real startX, real startY, real startZ, real endX, real endY, real endZ, real red, real green, real blue, real alpha, real width returns integer
    local string payload = "warvk:v1;lightning.create" + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(alpha) + ";r:" + R2S(width)
    return GetLocalizedHotkey(payload)
endfunction

// ---------------------------------------------------------------------------
// 数学程序与参数曲线（Phase 1）
//
// 公式是无循环、无递归、无副作用的纯表达式；ASCII 最长 384 字节，
// 不能包含分号（分号是 WarVK v1 的传输分隔符）。编译只发生一次，随后
// 曲线和闪电模板持有不可变字节码快照，不会逐点回调 JASS。
// ---------------------------------------------------------------------------

// 成功返回正整数 programId，失败返回 0；详细编译错误使用
// WarVKGetMathProgramLastError。常用内置量：pi、tau、t、time、length、seed、
// start/end/center/direction/forward/right/up、index/segments、
// branchIndex/branchDepth。公式中的其他合法标识符会成为可设置的 real 参数。
function WarVKCompileMathProgram takes string expression returns integer
    local string payload = "warvk:v1;math.program.compile" + ";s:" + expression
    return GetLocalizedHotkey(payload)
endfunction

function WarVKDestroyMathProgram takes integer programId returns nothing
    local string payload = "warvk:v1;math.program.destroy" + ";d:" + I2S(programId)
    call Preloader(payload)
endfunction

function WarVKIsMathProgramAlive takes integer programId returns boolean
    local string payload = "warvk:v1;math.program.isAlive" + ";d:" + I2S(programId)
    return GetLocalizedHotkey(payload) != 0
endfunction

function WarVKGetMathProgramLastError takes nothing returns string
    local string payload = "warvk:v1;math.program.lastError"
    return GetLocalizedString(payload)
endfunction

// 对标量公式求值并直接返回 JASS real。curveId 由 WarVKCreateCurve 创建，
// 公式参数先用 WarVKSetCurveReal 写入；t 范围0..1，time/seed 为本次求值输入。
function WarVKEvaluateMathReal takes integer curveId, real t, real time, integer seed returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedMathEvaluateReal)
        call SaveInteger(wvkTypedTable, transaction, 0, curveId)
        call SaveReal(wvkTypedTable, transaction, 1, t)
        call SaveReal(wvkTypedTable, transaction, 2, time)
        call SaveInteger(wvkTypedTable, transaction, 3, seed)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;math.evaluateReal" + ";d:" + I2S(curveId) + ";r:" + R2S(t) + ";r:" + R2S(time) + ";i:" + I2S(seed)
    return S2R(GetLocalizedString(payload))
endfunction

// 对同一标量公式求值并返回 JASS integer。roundingMode 使用
// WARVK_MATH_ROUND_*；结果超出 int32 或公式不是 scalar 时调用失败。
function WarVKEvaluateMathInteger takes integer curveId, real t, real time, integer seed, integer roundingMode returns integer
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedMathEvaluateInteger)
        call SaveInteger(wvkTypedTable, transaction, 0, curveId)
        call SaveReal(wvkTypedTable, transaction, 1, t)
        call SaveReal(wvkTypedTable, transaction, 2, time)
        call SaveInteger(wvkTypedTable, transaction, 3, seed)
        call SaveInteger(wvkTypedTable, transaction, 4, roundingMode)
        return LoadInteger(wvkTypedTable, transaction, wvkTypedQueryIntegerChild)
    endif
    set payload = "warvk:v1;math.evaluateInteger" + ";d:" + I2S(curveId) + ";r:" + R2S(t) + ";r:" + R2S(time) + ";i:" + I2S(seed) + ";i:" + I2S(roundingMode)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKCreateCurve takes integer programId returns integer
    local string payload = "warvk:v1;curve.create" + ";d:" + I2S(programId)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKDestroyCurve takes integer curveId returns nothing
    local string payload = "warvk:v1;curve.destroy" + ";d:" + I2S(curveId)
    call Preloader(payload)
endfunction

// name 必须是公式中出现的 ASCII 参数标识符（区分大小写）。
function WarVKSetCurveReal takes integer curveId, string name, real value returns nothing
    local string payload = "warvk:v1;curve.setReal" + ";d:" + I2S(curveId) + ";s:" + name + ";r:" + R2S(value)
    call Preloader(payload)
endfunction

function WarVKSetCurveCoordinateMode takes integer curveId, integer mode returns nothing
    local string payload = "warvk:v1;curve.setCoordinateMode" + ";d:" + I2S(curveId) + ";i:" + I2S(mode)
    call Preloader(payload)
endfunction

// lockStart/lockEnd 会给作者输出施加端点遮罩；OFFSET 模式推荐始终开启。
function WarVKSetCurveEndpointLocks takes integer curveId, boolean lockStart, boolean lockEnd returns nothing
    local string payload = "warvk:v1;curve.setEndpointLocks" + ";d:" + I2S(curveId) + ";b:" + WarVKBoolToken(lockStart) + ";b:" + WarVKBoolToken(lockEnd)
    call Preloader(payload)
endfunction

// 低频查询/调试接口；闪电渲染不会经 JASS 逐点调用它。
function WarVKEvaluateCurveComponent takes integer curveId, integer component, real t, real time, real startX, real startY, real startZ, real endX, real endY, real endZ, integer seed returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedCurveEvaluateComponent)
        call SaveInteger(wvkTypedTable, transaction, 0, curveId)
        call SaveInteger(wvkTypedTable, transaction, 1, component)
        call SaveReal(wvkTypedTable, transaction, 2, t)
        call SaveReal(wvkTypedTable, transaction, 3, time)
        call SaveReal(wvkTypedTable, transaction, 4, startX)
        call SaveReal(wvkTypedTable, transaction, 5, startY)
        call SaveReal(wvkTypedTable, transaction, 6, startZ)
        call SaveReal(wvkTypedTable, transaction, 7, endX)
        call SaveReal(wvkTypedTable, transaction, 8, endY)
        call SaveReal(wvkTypedTable, transaction, 9, endZ)
        call SaveInteger(wvkTypedTable, transaction, 10, seed)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;curve.evaluateComponent" + ";d:" + I2S(curveId) + ";i:" + I2S(component) + ";r:" + R2S(t) + ";r:" + R2S(time) + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ) + ";i:" + I2S(seed)
    return S2R(GetLocalizedString(payload))
endfunction

function WarVKEvaluateCurveDerivativeComponent takes integer curveId, integer component, real t, real time, real startX, real startY, real startZ, real endX, real endY, real endZ, integer seed returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedCurveDerivativeComponent)
        call SaveInteger(wvkTypedTable, transaction, 0, curveId)
        call SaveInteger(wvkTypedTable, transaction, 1, component)
        call SaveReal(wvkTypedTable, transaction, 2, t)
        call SaveReal(wvkTypedTable, transaction, 3, time)
        call SaveReal(wvkTypedTable, transaction, 4, startX)
        call SaveReal(wvkTypedTable, transaction, 5, startY)
        call SaveReal(wvkTypedTable, transaction, 6, startZ)
        call SaveReal(wvkTypedTable, transaction, 7, endX)
        call SaveReal(wvkTypedTable, transaction, 8, endY)
        call SaveReal(wvkTypedTable, transaction, 9, endZ)
        call SaveInteger(wvkTypedTable, transaction, 10, seed)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;curve.derivativeComponent" + ";d:" + I2S(curveId) + ";i:" + I2S(component) + ";r:" + R2S(t) + ";r:" + R2S(time) + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ) + ";i:" + I2S(seed)
    return S2R(GetLocalizedString(payload))
endfunction

function WarVKGetCurveArcLength takes integer curveId, real time, real startX, real startY, real startZ, real endX, real endY, real endZ, integer seed, integer samples returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedCurveArcLength)
        call SaveInteger(wvkTypedTable, transaction, 0, curveId)
        call SaveReal(wvkTypedTable, transaction, 1, time)
        call SaveReal(wvkTypedTable, transaction, 2, startX)
        call SaveReal(wvkTypedTable, transaction, 3, startY)
        call SaveReal(wvkTypedTable, transaction, 4, startZ)
        call SaveReal(wvkTypedTable, transaction, 5, endX)
        call SaveReal(wvkTypedTable, transaction, 6, endY)
        call SaveReal(wvkTypedTable, transaction, 7, endZ)
        call SaveInteger(wvkTypedTable, transaction, 8, seed)
        call SaveInteger(wvkTypedTable, transaction, 9, samples)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;curve.arcLength" + ";d:" + I2S(curveId) + ";r:" + R2S(time) + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ) + ";i:" + I2S(seed) + ";i:" + I2S(samples)
    return S2R(GetLocalizedString(payload))
endfunction

// 创建点曲线的有界上传记录。expectedPointCount 必须为 2..1024。
// 创建后依次 Append，最后 Finalize；冻结前不能绑定给闪电。
function WarVKCreatePointCurve takes integer expectedPointCount returns integer
    local string payload = "warvk:v1;curve.points.create" + ";i:" + I2S(expectedPointCount)
    return GetLocalizedHotkey(payload)
endfunction

// 每次最多上传四个世界坐标点，pointCount 为 1..4。最后一批不足四点时，
// 未使用的坐标仍需传入任意有限值（推荐重复最后一个有效点），底层会忽略它们。
function WarVKAppendPointCurve4 takes integer curveId, integer pointCount, real x0, real y0, real z0, real x1, real y1, real z1, real x2, real y2, real z2, real x3, real y3, real z3 returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedCurvePointAppend4)
        call SaveInteger(wvkTypedTable, transaction, 0, curveId)
        call SaveInteger(wvkTypedTable, transaction, 1, pointCount)
        call SaveReal(wvkTypedTable, transaction, 2, x0)
        call SaveReal(wvkTypedTable, transaction, 3, y0)
        call SaveReal(wvkTypedTable, transaction, 4, z0)
        call SaveReal(wvkTypedTable, transaction, 5, x1)
        call SaveReal(wvkTypedTable, transaction, 6, y1)
        call SaveReal(wvkTypedTable, transaction, 7, z1)
        call SaveReal(wvkTypedTable, transaction, 8, x2)
        call SaveReal(wvkTypedTable, transaction, 9, y2)
        call SaveReal(wvkTypedTable, transaction, 10, z2)
        call SaveReal(wvkTypedTable, transaction, 11, x3)
        call SaveReal(wvkTypedTable, transaction, 12, y3)
        call SaveReal(wvkTypedTable, transaction, 13, z3)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedCurvePointAppend4)
        return
    endif
    set payload = "warvk:v1;curve.points.append4" + ";d:" + I2S(curveId) + ";i:" + I2S(pointCount) + ";r:" + R2S(x0) + ";r:" + R2S(y0) + ";r:" + R2S(z0) + ";r:" + R2S(x1) + ";r:" + R2S(y1) + ";r:" + R2S(z1) + ";r:" + R2S(x2) + ";r:" + R2S(y2) + ";r:" + R2S(z2) + ";r:" + R2S(x3) + ";r:" + R2S(y3) + ";r:" + R2S(z3)
    call Preloader(payload)
endfunction

// 只有累计上传点数恰好等于 expectedPointCount 时才能冻结。冻结会一次性
// 建立累计弧长表并发布不可变快照，渲染线程永远看不到半条曲线。
function WarVKFinalizePointCurve takes integer curveId returns nothing
    local string payload = "warvk:v1;curve.points.finalize" + ";d:" + I2S(curveId)
    call Preloader(payload)
endfunction

// ---------------------------------------------------------------------------
// 闪电模板（地图侧编写的 CPU 描述符）
//
// 推荐流程（地图初始化 / YDWE 触发器）：
//   1) templateId = WarVKCreateLightningTemplate("MyBolt")
//   2) WarVKSetLightningTemplateBasic(...)
//   3) WarVKSetLightningTemplateAdvanced(...)   // 强烈建议
//   4) WarVKSetLightningTemplateOptional(...)   // 可选
//   5) WarVKSetLightningTemplateFormulaCurve(...) // 公式曲线，可选
//   6) WarVKFinalizeLightningTemplate(templateId)
//   7) id = WarVKCreateLightningFromTemplate(templateId, x1,y1,z1, x2,y2,z2, seed)
//
// 模板配置只改动 DXVK 持有的 CPU 状态，不需要已有渲染帧。
// 贴图在 D3D9 设备就绪后的首次绘制时再加载。
// Finalize 会冻结描述符：之后再改会被拒绝；实例拷贝冻结值，互不影响。
// ---------------------------------------------------------------------------

// name：作者可读的模板名；中文、英文均可，推荐填写有意义的名称。
//       返回的 templateId 才是模板的唯一标识；名称仅用于创建时的诊断。
//       v1 传输层保持 ASCII 安全，因此这里会把名称转换为以 T 开头的
//       StringHash 标识。这样 YDWE 中填写中文不会让模板创建返回 0。
function WarVKMakeLightningTemplateWireName takes string name returns string
    return "T" + I2S(StringHash(name))
endfunction

// 返回：成功为正整数模板 id；失败为 0（用 WarVKGetLastError 查原因）。
function WarVKCreateLightningTemplate takes string name returns integer
    local string payload = "warvk:v1;lightning.template.create" + ";s:" + WarVKMakeLightningTemplateWireName(name)
    return GetLocalizedHotkey(payload)
endfunction

// templateId：WarVKCreateLightningTemplate 返回的 id（尚未 Finalize）。
// texturePath：相对地图/MPQ 路径（不能为空）。支持 BLP1 JPEG/索引色、
//              TGA、PNG、JPG/JPEG、BMP。暂不支持 BLP2。BLP1 JPEG 本身
//              不保存 Alpha，闪电渲染器会从贴图边缘背景自动生成软遮罩；
//              若需要精确透明形状，请使用带 Alpha 的 PNG/TGA。
//              例："ReplaceableTextures\\Weather\\Lightning.blp"
//              或 "war3mapImported\\Lightning.blp"
// startRed/startGreen/startBlue：起点颜色 RGB，建议 0..1（须为有限数且 >= 0）。
// startAlpha：起点不透明度，硬限制 0..1（不要填 255）。
// endRed/endGreen/endBlue/endAlpha：终点颜色，规则同起点。
// startWidth/endWidth：两端世界坐标带宽，范围 1..4096。
// uvTiling：贴图沿电弧重复次数，范围 0.01..128。
// uvScrollSpeed：贴图沿电弧滚动速度，范围 -128..128（负值反向）。
// renderMode：使用 WARVK_LIGHTNING_RENDER_* 常量：
//   0 ALPHA 无深度、1 ALPHA 深度测试、2 加法无深度、3 加法深度测试。
//   始终不写深度；仅 1/3 开启深度测试。
// 推荐起步：颜色 (0.20,0.70,1.00,0.95)->(0.90,0.98,1.00,0.75)，
//   宽度 30/12，UV 3/0.85，renderMode WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH。
function WarVKSetLightningTemplateBasic takes integer templateId, string texturePath, real startRed, real startGreen, real startBlue, real startAlpha, real endRed, real endGreen, real endBlue, real endAlpha, real startWidth, real endWidth, real uvTiling, real uvScrollSpeed, integer renderMode returns nothing
    local string payload = "warvk:v1;lightning.template.setBasic" + ";d:" + I2S(templateId) + ";s:" + texturePath + ";r:" + R2S(startRed) + ";r:" + R2S(startGreen) + ";r:" + R2S(startBlue) + ";r:" + R2S(startAlpha) + ";r:" + R2S(endRed) + ";r:" + R2S(endGreen) + ";r:" + R2S(endBlue) + ";r:" + R2S(endAlpha) + ";r:" + R2S(startWidth) + ";r:" + R2S(endWidth) + ";r:" + R2S(uvTiling) + ";r:" + R2S(uvScrollSpeed) + ";i:" + I2S(renderMode)
    call Preloader(payload)
endfunction

// templateId：尚未 Finalize 的模板 id。
// averageSegmentLength：每段折线目标世界长度，范围 4..8192。
//                       越小越密、锯齿更细，开销更大。
// minimumSegments/maximumSegments：段数钳制，各自 2..64，且 max >= min。
// curveAmplitude：大尺度弯曲偏移（世界单位），范围 -4096..4096。
// noiseAmplitude：分形噪声位移幅度，范围 0..4096。
// noiseFrequency：噪声空间频率，范围 0..64（越大扭动越密）。
// noiseScrollSpeed：噪声相位随时间滚动，范围 -64..64。
// noiseOctaves：噪声层数，整数 1..4（越大细节越多、开销越大）。
// branchCount：侧向分叉数，整数 0..8（0=仅主电）。
// branchLengthScale：分叉相对主电长度，范围 0..8（常用约 1.0）。
// branchWidthScale：分叉相对主电宽度，范围 0.05..4（常用约 0.45）。
// 推荐起步：80, 4, 32, 70, 36, 9, 1, 2, 2, 1.00, 0.45。
function WarVKSetLightningTemplateAdvanced takes integer templateId, real averageSegmentLength, integer minimumSegments, integer maximumSegments, real curveAmplitude, real noiseAmplitude, real noiseFrequency, real noiseScrollSpeed, integer noiseOctaves, integer branchCount, real branchLengthScale, real branchWidthScale returns nothing
    local string payload = "warvk:v1;lightning.template.setAdvanced" + ";d:" + I2S(templateId) + ";r:" + R2S(averageSegmentLength) + ";i:" + I2S(minimumSegments) + ";i:" + I2S(maximumSegments) + ";r:" + R2S(curveAmplitude) + ";r:" + R2S(noiseAmplitude) + ";r:" + R2S(noiseFrequency) + ";r:" + R2S(noiseScrollSpeed) + ";i:" + I2S(noiseOctaves) + ";i:" + I2S(branchCount) + ";r:" + R2S(branchLengthScale) + ";r:" + R2S(branchWidthScale)
    call Preloader(payload)
endfunction

// templateId：尚未 Finalize 的模板 id。
// lifetimeSec：总寿命（秒）；0=常驻直到 WarVKDestroyLightning。
//              范围 0..600。当 >0 时，fadeIn/fadeOut 各自不能超过 lifetimeSec。
// fadeInSec/fadeOutSec：淡入/淡出秒数，范围 0..600。
// pulseAmplitude：亮度脉冲强度，范围 0..1。
// pulseFrequency：脉冲频率（次/秒），范围 0..120。
// pulseTravelSpeed：脉冲沿电弧传播速度，范围 -120..120（0=原地脉冲）。
// flickerAmplitude：随机闪烁强度，范围 0..1。
// flickerFrequencyHz：闪烁频率（Hz），范围 0..120。
// glowWidthScale：外层柔光宽度倍率，范围 1..8（1=不比主电更宽）。
// glowOpacity：柔光层不透明度，范围 0..1（0=关闭辉光）。
// 推荐常驻特效：寿命 0，淡入淡出 0.08/0.16，脉冲 0.25/7/0，
//   闪烁 0.08/11，辉光 1.80/0.22。
function WarVKSetLightningTemplateOptional takes integer templateId, real lifetimeSec, real fadeInSec, real fadeOutSec, real pulseAmplitude, real pulseFrequency, real pulseTravelSpeed, real flickerAmplitude, real flickerFrequencyHz, real glowWidthScale, real glowOpacity returns nothing
    local string payload = "warvk:v1;lightning.template.setOptional" + ";d:" + I2S(templateId) + ";r:" + R2S(lifetimeSec) + ";r:" + R2S(fadeInSec) + ";r:" + R2S(fadeOutSec) + ";r:" + R2S(pulseAmplitude) + ";r:" + R2S(pulseFrequency) + ";r:" + R2S(pulseTravelSpeed) + ";r:" + R2S(flickerAmplitude) + ";r:" + R2S(flickerFrequencyHz) + ";r:" + R2S(glowWidthScale) + ";r:" + R2S(glowOpacity)
    call Preloader(payload)
endfunction

// 把当前 curve 参数做成不可变快照并绑定到尚未冻结的闪电模板。
// 后续修改/销毁 curve 或 program 不会改变已经绑定的模板。
function WarVKSetLightningTemplateFormulaCurve takes integer templateId, integer curveId returns nothing
    local string payload = "warvk:v1;lightning.template.setFormulaCurve" + ";d:" + I2S(templateId) + ";d:" + I2S(curveId)
    call Preloader(payload)
endfunction

// templateId：已配置基础（通常也已配置进阶）的模板 id。
// 冻结模板：之后 setBasic/setAdvanced/setOptional/setFormulaCurve 均会被拒绝。
// 必须在 WarVKCreateLightningFromTemplate 之前调用。
function WarVKFinalizeLightningTemplate takes integer templateId returns nothing
    local string payload = "warvk:v1;lightning.template.finalize" + ";d:" + I2S(templateId)
    call Preloader(payload)
endfunction

// templateId：已 Finalize 的模板 id。
// startX/Y/Z、endX/Y/Z：电弧两端的有限世界坐标（两端不要重合过近）。
// seed：噪声/分叉随机种子。0=稳定自动种子；非 0 时同种子可复现同一锯齿形态
//       （便于联机一致或重复施法形态一致）。
// 返回：成功为正整数闪电实例 id；失败为 0。
function WarVKCreateLightningFromTemplate takes integer templateId, real startX, real startY, real startZ, real endX, real endY, real endZ, integer seed returns integer
    local string payload = "warvk:v1;lightning.createFromTemplate" + ";d:" + I2S(templateId) + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ) + ";i:" + I2S(seed)
    return GetLocalizedHotkey(payload)
endfunction

// 用已冻结的世界坐标点曲线创建一个闪电实例。整条曲线由一个连续 Ribbon
// 绘制（可选辉光会增加第二次 draw），不会为每一小段创建托管闪电对象。
function WarVKCreatePolylineLightning takes integer templateId, integer curveId, integer seed returns integer
    local string payload = "warvk:v1;lightning.createPolylineFromTemplate" + ";d:" + I2S(templateId) + ";d:" + I2S(curveId) + ";i:" + I2S(seed)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKDestroyLightning takes integer lightningId returns nothing
    local string payload = "warvk:v1;lightning.destroy" + ";d:" + I2S(lightningId)
    call Preloader(payload)
endfunction

function WarVKSetLightningEnabled takes integer lightningId, boolean enabled returns nothing
    local string payload = "warvk:v1;lightning.setEnabled" + ";d:" + I2S(lightningId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

function WarVKSetLightningEndpoints takes integer lightningId, real startX, real startY, real startZ, real endX, real endY, real endZ returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedLightningSetEndpoints)
        call SaveInteger(wvkTypedTable, transaction, 0, lightningId)
        call SaveReal(wvkTypedTable, transaction, 1, startX)
        call SaveReal(wvkTypedTable, transaction, 2, startY)
        call SaveReal(wvkTypedTable, transaction, 3, startZ)
        call SaveReal(wvkTypedTable, transaction, 4, endX)
        call SaveReal(wvkTypedTable, transaction, 5, endY)
        call SaveReal(wvkTypedTable, transaction, 6, endZ)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedLightningSetEndpoints)
        return
    endif
    set payload = "warvk:v1;lightning.setEndpoints" + ";d:" + I2S(lightningId) + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ)
    call Preloader(payload)
endfunction

function WarVKSetLightningColor takes integer lightningId, real red, real green, real blue, real alpha returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedLightningSetColor)
        call SaveInteger(wvkTypedTable, transaction, 0, lightningId)
        call SaveReal(wvkTypedTable, transaction, 1, red)
        call SaveReal(wvkTypedTable, transaction, 2, green)
        call SaveReal(wvkTypedTable, transaction, 3, blue)
        call SaveReal(wvkTypedTable, transaction, 4, alpha)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedLightningSetColor)
        return
    endif
    set payload = "warvk:v1;lightning.setColor" + ";d:" + I2S(lightningId) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(alpha)
    call Preloader(payload)
endfunction

function WarVKSetLightningWidth takes integer lightningId, real width returns nothing
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedLightningSetWidth)
        call SaveInteger(wvkTypedTable, transaction, 0, lightningId)
        call SaveReal(wvkTypedTable, transaction, 1, width)
        call SaveInteger(wvkTypedTable, transaction, wvkTypedCommitChild, wvkTypedLightningSetWidth)
        return
    endif
    set payload = "warvk:v1;lightning.setWidth" + ";d:" + I2S(lightningId) + ";r:" + R2S(width)
    call Preloader(payload)
endfunction

// 给现有闪电实例绑定当前 curve 的不可变快照。
function WarVKSetLightningFormulaCurve takes integer lightningId, integer curveId returns nothing
    local string payload = "warvk:v1;lightning.setFormulaCurve" + ";d:" + I2S(lightningId) + ";d:" + I2S(curveId)
    call Preloader(payload)
endfunction

// 原子替换现有闪电实例的完整点曲线快照；适合低频重建动态轨迹。
function WarVKSetPolylineLightningCurve takes integer lightningId, integer curveId returns nothing
    local string payload = "warvk:v1;lightning.setPolylineCurve" + ";d:" + I2S(lightningId) + ";d:" + I2S(curveId)
    call Preloader(payload)
endfunction

function WarVKIsLightningAlive takes integer lightningId returns boolean
    local string payload = "warvk:v1;lightning.isAlive" + ";d:" + I2S(lightningId)
    return GetLocalizedHotkey(payload) != 0
endfunction

function WarVKGetManagedObjectCount takes nothing returns integer
    local string payload = "warvk:v1;managedObject.count"
    return GetLocalizedHotkey(payload)
endfunction

function WarVKIsManagedObjectAlive takes integer objectId returns boolean
    local string payload = "warvk:v1;managedObject.isAlive" + ";d:" + I2S(objectId)
    return GetLocalizedHotkey(payload) != 0
endfunction

function WarVKGetManagedObjectType takes integer objectId returns integer
    local string payload = "warvk:v1;managedObject.type" + ";d:" + I2S(objectId)
    return GetLocalizedHotkey(payload)
endfunction

function WarVKGetVisualTimeSeconds takes nothing returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedTimeVisualSeconds)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;time.visualSeconds"
    return S2R(GetLocalizedString(payload))
endfunction

function WarVKGetFrameIndex takes nothing returns integer
    local string payload = "warvk:v1;time.frameIndex"
    return GetLocalizedHotkey(payload)
endfunction

function WarVKGetFramesPerSecond takes nothing returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedStatsFramesPerSecond)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;stats.framesPerSecond"
    return S2R(GetLocalizedString(payload))
endfunction

function WarVKGetFrameTimeMilliseconds takes nothing returns real
    local integer transaction = 0
    local string payload
    if WVKTypedReady() then
        set transaction = WVKTypedBegin(wvkTypedStatsFrameTimeMilliseconds)
        return LoadReal(wvkTypedTable, transaction, wvkTypedQueryRealChild)
    endif
    set payload = "warvk:v1;stats.frameTimeMilliseconds"
    return S2R(GetLocalizedString(payload))
endfunction

function WarVKGetDrawCallCount takes nothing returns integer
    local string payload = "warvk:v1;stats.drawCallCount"
    return GetLocalizedHotkey(payload)
endfunction
