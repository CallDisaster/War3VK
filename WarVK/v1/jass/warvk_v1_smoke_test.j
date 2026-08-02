// Optional local visual smoke test. Import after warvk_v1.j or
// warvk_v1_complete.j. These functions change only WarVK-owned local
// rendering state.
//
// Keep the returned id alive for at least one rendered frame. After taking a
// screenshot, pass that id to WarVKFinishPointLightSmokeTest.
function WarVKBeginPointLightSmokeTest takes real x, real y, real z returns integer
    local integer before = WarVKGetManagedObjectCount()
    local integer lightId = WarVKCreatePointLight(x, y, z, 900.00, 0.25, 0.70, 1.00, 4.00)
    call BJDebugMsg("WarVK version=" + WarVKGetVersion())
    call BJDebugMsg("WarVK protocol=" + I2S(WarVKGetProtocolVersion()) + " flags=" + I2S(WarVKGetFeatureFlags()))
    if lightId > 0 then
        call WarVKSetPointLightPosition(lightId, x, y, z)
        call WarVKSetPointLightColorIntensity(lightId, 0.25, 0.70, 1.00, 4.00)
        call WarVKSetPointLightRadius(lightId, 900.00)
        call WarVKSetPointLightShadowEnabled(lightId, true)
        call WarVKSetPointLightShadowConfig(lightId, 1024, 0.05)
        call BJDebugMsg("WarVK point light id=" + I2S(lightId) + " count=" + I2S(WarVKGetManagedObjectCount()) + " before=" + I2S(before))
        call BJDebugMsg("Keep this light for a screenshot, then call WarVKFinishPointLightSmokeTest.")
    else
        call BJDebugMsg("WarVK point light create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
    endif
    return lightId
endfunction

function WarVKFinishPointLightSmokeTest takes integer lightId returns nothing
    local integer before = WarVKGetManagedObjectCount()
    if lightId > 0 and WarVKIsPointLightAlive(lightId) then
        call WarVKDestroyPointLight(lightId)
        call BJDebugMsg("WarVK point light removed; count=" + I2S(WarVKGetManagedObjectCount()) + " before=" + I2S(before))
    else
        call BJDebugMsg("WarVK smoke cleanup skipped: point light id is not alive.")
    endif
    set lightId = 0
endfunction
