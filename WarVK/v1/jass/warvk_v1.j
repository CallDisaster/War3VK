// Generated from the WarVK v1 manifest. Staging wrapper only; do not edit.
// 警告：所有 WarVK 调用与查询仅代表本机视觉状态。
// 严禁将任何返回值用于多人同步玩法分支、随机数、单位订单或同步状态判断。

function WarVKBoolToken takes boolean value returns string
    if value then
        return "1"
    endif
    return "0"
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetVersion takes nothing returns string
    local string payload = "warvk:v1;system.version"
    return GetLocalizedString(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetProtocolVersion takes nothing returns integer
    local string payload = "warvk:v1;system.protocolVersion"
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetLastErrorCode takes nothing returns integer
    local string payload = "warvk:v1;system.lastErrorCode"
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetLastError takes nothing returns string
    local string payload = "warvk:v1;system.lastError"
    return GetLocalizedString(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKClearError takes nothing returns nothing
    local string payload = "warvk:v1;system.clearError"
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetFeatureFlags takes nothing returns integer
    local string payload = "warvk:v1;system.featureFlags"
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKIsRuntimeReady takes nothing returns boolean
    local string payload = "warvk:v1;system.runtimeReady"
    return GetLocalizedHotkey(payload) != 0
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetSunEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;sun.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetSunDirection takes real x, real y, real z returns nothing
    local string payload = "warvk:v1;sun.setDirection" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetSunColorIntensity takes real red, real green, real blue, real intensity returns nothing
    local string payload = "warvk:v1;sun.setColorIntensity" + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(intensity)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetCsmEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;csm.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetCsmLayout takes integer cascadeCount, real maxDistance returns nothing
    local string payload = "warvk:v1;csm.setLayout" + ";i:" + I2S(cascadeCount) + ";r:" + R2S(maxDistance)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetCsmTuning takes real depthBias, real blendWidth returns nothing
    local string payload = "warvk:v1;csm.setTuning" + ";r:" + R2S(depthBias) + ";r:" + R2S(blendWidth)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKCreatePointLight takes real x, real y, real z, real radius, real red, real green, real blue, real intensity returns integer
    local string payload = "warvk:v1;pointLight.create" + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z) + ";r:" + R2S(radius) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(intensity)
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKDestroyPointLight takes integer lightId returns nothing
    local string payload = "warvk:v1;pointLight.destroy" + ";d:" + I2S(lightId)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPointLightEnabled takes integer lightId, boolean enabled returns nothing
    local string payload = "warvk:v1;pointLight.setEnabled" + ";d:" + I2S(lightId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPointLightPosition takes integer lightId, real x, real y, real z returns nothing
    local string payload = "warvk:v1;pointLight.setPosition" + ";d:" + I2S(lightId) + ";r:" + R2S(x) + ";r:" + R2S(y) + ";r:" + R2S(z)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPointLightColorIntensity takes integer lightId, real red, real green, real blue, real intensity returns nothing
    local string payload = "warvk:v1;pointLight.setColorIntensity" + ";d:" + I2S(lightId) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(intensity)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPointLightRadius takes integer lightId, real radius returns nothing
    local string payload = "warvk:v1;pointLight.setRadius" + ";d:" + I2S(lightId) + ";r:" + R2S(radius)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPointLightShadowEnabled takes integer lightId, boolean enabled returns nothing
    local string payload = "warvk:v1;pointLight.setShadowEnabled" + ";d:" + I2S(lightId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPointLightShadowConfig takes integer lightId, integer resolution, real bias returns nothing
    local string payload = "warvk:v1;pointLight.setShadowConfig" + ";d:" + I2S(lightId) + ";i:" + I2S(resolution) + ";r:" + R2S(bias)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKIsPointLightAlive takes integer lightId returns boolean
    local string payload = "warvk:v1;pointLight.isAlive" + ";d:" + I2S(lightId)
    return GetLocalizedHotkey(payload) != 0
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetVolumetricEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;volumetric.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetVolumetricDensity takes real density returns nothing
    local string payload = "warvk:v1;volumetric.setDensity" + ";r:" + R2S(density)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetVolumetricScattering takes real scattering, real anisotropy returns nothing
    local string payload = "warvk:v1;volumetric.setScattering" + ";r:" + R2S(scattering) + ";r:" + R2S(anisotropy)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetVolumetricQuality takes integer stepCount, real maxDistance returns nothing
    local string payload = "warvk:v1;volumetric.setQuality" + ";i:" + I2S(stepCount) + ";r:" + R2S(maxDistance)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetOutlineEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;outline.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetOutlineColor takes real red, real green, real blue, real alpha returns nothing
    local string payload = "warvk:v1;outline.setColor" + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(alpha)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetOutlineParameters takes real width, real depthThreshold returns nothing
    local string payload = "warvk:v1;outline.setParameters" + ";r:" + R2S(width) + ";r:" + R2S(depthThreshold)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetBloomEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;bloom.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetBloomParameters takes real threshold, real intensity, real softKnee returns nothing
    local string payload = "warvk:v1;bloom.setParameters" + ";r:" + R2S(threshold) + ";r:" + R2S(intensity) + ";r:" + R2S(softKnee)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetBloomRadius takes real radius returns nothing
    local string payload = "warvk:v1;bloom.setRadius" + ";r:" + R2S(radius)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPostfxEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;postfx.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPostfxExposureGamma takes real exposure, real gamma returns nothing
    local string payload = "warvk:v1;postfx.setExposureGamma" + ";r:" + R2S(exposure) + ";r:" + R2S(gamma)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetPostfxColorGrade takes real saturation, real contrast returns nothing
    local string payload = "warvk:v1;postfx.setColorGrade" + ";r:" + R2S(saturation) + ";r:" + R2S(contrast)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetAaMode takes integer mode, integer quality returns nothing
    local string payload = "warvk:v1;aa.setMode" + ";i:" + I2S(mode) + ";i:" + I2S(quality)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetAaSharpness takes real sharpness returns nothing
    local string payload = "warvk:v1;aa.setSharpness" + ";r:" + R2S(sharpness)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetDayNightEnabled takes boolean enabled returns nothing
    local string payload = "warvk:v1;dayNight.setEnabled" + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetDayNightTime takes real hours returns nothing
    local string payload = "warvk:v1;dayNight.setTime" + ";r:" + R2S(hours)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetDayNightSpeed takes real scale returns nothing
    local string payload = "warvk:v1;dayNight.setSpeed" + ";r:" + R2S(scale)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKCreateLightning takes real startX, real startY, real startZ, real endX, real endY, real endZ, real red, real green, real blue, real alpha, real width returns integer
    local string payload = "warvk:v1;lightning.create" + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(alpha) + ";r:" + R2S(width)
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKDestroyLightning takes integer lightningId returns nothing
    local string payload = "warvk:v1;lightning.destroy" + ";d:" + I2S(lightningId)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetLightningEnabled takes integer lightningId, boolean enabled returns nothing
    local string payload = "warvk:v1;lightning.setEnabled" + ";d:" + I2S(lightningId) + ";b:" + WarVKBoolToken(enabled)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetLightningEndpoints takes integer lightningId, real startX, real startY, real startZ, real endX, real endY, real endZ returns nothing
    local string payload = "warvk:v1;lightning.setEndpoints" + ";d:" + I2S(lightningId) + ";r:" + R2S(startX) + ";r:" + R2S(startY) + ";r:" + R2S(startZ) + ";r:" + R2S(endX) + ";r:" + R2S(endY) + ";r:" + R2S(endZ)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetLightningColor takes integer lightningId, real red, real green, real blue, real alpha returns nothing
    local string payload = "warvk:v1;lightning.setColor" + ";d:" + I2S(lightningId) + ";r:" + R2S(red) + ";r:" + R2S(green) + ";r:" + R2S(blue) + ";r:" + R2S(alpha)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKSetLightningWidth takes integer lightningId, real width returns nothing
    local string payload = "warvk:v1;lightning.setWidth" + ";d:" + I2S(lightningId) + ";r:" + R2S(width)
    call Preloader(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKIsLightningAlive takes integer lightningId returns boolean
    local string payload = "warvk:v1;lightning.isAlive" + ";d:" + I2S(lightningId)
    return GetLocalizedHotkey(payload) != 0
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetManagedObjectCount takes nothing returns integer
    local string payload = "warvk:v1;managedObject.count"
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKIsManagedObjectAlive takes integer objectId returns boolean
    local string payload = "warvk:v1;managedObject.isAlive" + ";d:" + I2S(objectId)
    return GetLocalizedHotkey(payload) != 0
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetManagedObjectType takes integer objectId returns integer
    local string payload = "warvk:v1;managedObject.type" + ";d:" + I2S(objectId)
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetVisualTimeSeconds takes nothing returns real
    local string payload = "warvk:v1;time.visualSeconds"
    return S2R(GetLocalizedString(payload))
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetFrameIndex takes nothing returns integer
    local string payload = "warvk:v1;time.frameIndex"
    return GetLocalizedHotkey(payload)
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetFramesPerSecond takes nothing returns real
    local string payload = "warvk:v1;stats.framesPerSecond"
    return S2R(GetLocalizedString(payload))
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetFrameTimeMilliseconds takes nothing returns real
    local string payload = "warvk:v1;stats.frameTimeMilliseconds"
    return S2R(GetLocalizedString(payload))
endfunction

// 仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。
function WarVKGetDrawCallCount takes nothing returns integer
    local string payload = "warvk:v1;stats.drawCallCount"
    return GetLocalizedHotkey(payload)
endfunction
