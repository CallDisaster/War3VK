#pragma once
#include "../warvk_bridge.j"

library WarVKRender
    function DXVK_GetPluginVersionString takes nothing returns string
        return WarVK_GetVersionString()
    endfunction

    function DXVK_GetPluginVersion takes nothing returns integer
        return WarVK_QueryIntegerRaw("plugin-version")
    endfunction

    function TestIntReturn takes nothing returns integer
        return WarVK_QueryIntegerRaw("ping")
    endfunction

    function TestFloatReturn takes nothing returns real
        if (WarVK_IsLoaded()) then
            return 1.0
        endif
        return 0.0
    endfunction

    function TestStringReturn takes nothing returns string
        return WarVK_GetVersionString()
    endfunction

    function TestBooleanReturn takes nothing returns boolean
        return WarVK_IsLoaded()
    endfunction

    function DXVK_GetGameTimeSeconds takes nothing returns real
        return I2R(WarVK_QueryIntegerRaw("game-time-ms")) / 1000.0
    endfunction

    function DXVK_GetTimeOfDay takes nothing returns real
        return I2R(WarVK_QueryIntegerRaw("game-time-ms")) / 1000.0
    endfunction

    function DXVK_GetGameVersion takes nothing returns integer
        return WarVK_QueryIntegerRaw("game-version")
    endfunction

    function AddPointLight takes real x, real y, real z, real range, real r, real g, real b, real intensity, real shadowIntensity returns integer
        return WarVK_QueryInteger("add-point-light|" + R2S(x) + "|" + R2S(y) + "|" + R2S(z) + "|" + R2S(range) + "|" + R2S(r) + "|" + R2S(g) + "|" + R2S(b) + "|" + R2S(intensity) + "|" + R2S(shadowIntensity))
    endfunction

    function UpdatePointLight takes integer id, real x, real y, real z, real range, real r, real g, real b, real intensity returns boolean
        return WarVK_QueryInteger("update-point-light|" + I2S(id) + "|" + R2S(x) + "|" + R2S(y) + "|" + R2S(z) + "|" + R2S(range) + "|" + R2S(r) + "|" + R2S(g) + "|" + R2S(b) + "|" + R2S(intensity)) == 1
    endfunction

    function RemovePointLight takes integer id returns boolean
        return WarVK_QueryInteger("remove-point-light|" + I2S(id)) == 1
    endfunction

    function DXVK_OutlineAddHandle takes integer handleId returns nothing
        call WarVK_Command("outline-add-handle|" + I2S(handleId))
    endfunction

    function DXVK_OutlineRemoveHandle takes integer handleId returns nothing
        call WarVK_Command("outline-remove-handle|" + I2S(handleId))
    endfunction

    function DXVK_OutlineClearHandles takes nothing returns nothing
        call WarVK_Command("outline-clear-handles")
    endfunction

    function DXVK_BloomAddHandle takes integer handleId, real boost returns nothing
        call WarVK_Command("bloom-add-handle|" + I2S(handleId) + "|" + R2S(boost))
    endfunction

    function DXVK_BloomRemoveHandle takes integer handleId returns nothing
        call WarVK_Command("bloom-remove-handle|" + I2S(handleId))
    endfunction

    function DXVK_BloomClearHandles takes nothing returns nothing
        call WarVK_Command("bloom-clear-handles")
    endfunction

    function DXVK_BloomGetBoost takes integer handleId returns real
        // The no-native bridge currently supports integer/string returns only.
        return 0.0
    endfunction

    function DXVK_SetLightingEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-lighting-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetSunDirection takes real x, real y, real z returns nothing
        call WarVK_Command("set-sun-direction|" + R2S(x) + "|" + R2S(y) + "|" + R2S(z))
    endfunction

    function DXVK_SetSunColor takes real r, real g, real b returns nothing
        call WarVK_Command("set-sun-color|" + R2S(r) + "|" + R2S(g) + "|" + R2S(b))
    endfunction

    function DXVK_SetSunIntensity takes real intensity returns nothing
        call WarVK_Command("set-sun-intensity|" + R2S(intensity))
    endfunction

    function DXVK_SetShadowEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-shadow-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetShadowStrength takes real strength returns nothing
        call WarVK_Command("set-shadow-strength|" + R2S(strength))
    endfunction

    function DXVK_SetShadowBias takes real bias returns nothing
        call WarVK_Command("set-shadow-bias|" + R2S(bias))
    endfunction

    function DXVK_SetShadowPcfRadius takes real radius returns nothing
        call WarVK_Command("set-shadow-pcf-radius|" + R2S(radius))
    endfunction

    function DXVK_SetShadowDebugMode takes integer mode returns nothing
        call WarVK_Command("set-shadow-debug-mode|" + I2S(mode))
    endfunction

    function DXVK_SetPointLightsEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-point-lights-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetPointShadowEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-point-shadow-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetOutlineEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-outline-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetOutlineWidth takes real widthPx returns nothing
        call WarVK_Command("set-outline-width|" + R2S(widthPx))
    endfunction

    function DXVK_SetOutlineColor takes real r, real g, real b, real a returns nothing
        call WarVK_Command("set-outline-color|" + R2S(r) + "|" + R2S(g) + "|" + R2S(b) + "|" + R2S(a))
    endfunction

    function DXVK_SetOutlineMode takes integer mode returns nothing
        call WarVK_Command("set-outline-mode|" + I2S(mode))
    endfunction

    function DXVK_SetOutlineVisibility takes boolean showVisible, boolean showOccluded returns nothing
        call WarVK_Command("set-outline-visibility|" + WarVK_BoolArg(showVisible) + "|" + WarVK_BoolArg(showOccluded))
    endfunction

    function DXVK_SetPostFxEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-postfx-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetExposure takes real exposure returns nothing
        call WarVK_Command("set-exposure|" + R2S(exposure))
    endfunction

    function DXVK_SetBloomEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-bloom-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetBloomParams takes real threshold, real softKnee, real intensity returns nothing
        call WarVK_Command("set-bloom-params|" + R2S(threshold) + "|" + R2S(softKnee) + "|" + R2S(intensity))
    endfunction

    function DXVK_SetAcesEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-aces-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetSsaoEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-ssao-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetSsaoParams takes real radiusPx, real strength, real bias, real power returns nothing
        call WarVK_Command("set-ssao-params|" + R2S(radiusPx) + "|" + R2S(strength) + "|" + R2S(bias) + "|" + R2S(power))
    endfunction

    function DXVK_SetAaMode takes integer mode returns nothing
        call WarVK_Command("set-aa-mode|" + I2S(mode))
    endfunction

    function DXVK_SetFxaaParams takes real subpix, real edgeThreshold, real edgeThresholdMin returns nothing
        call WarVK_Command("set-fxaa-params|" + R2S(subpix) + "|" + R2S(edgeThreshold) + "|" + R2S(edgeThresholdMin))
    endfunction

    function DXVK_SetSmaaParams takes real threshold, integer search, integer diagSearch returns nothing
        call WarVK_Command("set-smaa-params|" + R2S(threshold) + "|" + I2S(search) + "|" + I2S(diagSearch))
    endfunction

    function DXVK_SetDayNightEnabled takes boolean enabled returns nothing
        call WarVK_Command("set-day-night-enabled|" + WarVK_BoolArg(enabled))
    endfunction

    function DXVK_SetDayNightMinFactor takes real minFactor returns nothing
        call WarVK_Command("set-day-night-min-factor|" + R2S(minFactor))
    endfunction

    function DXVK_SetDayNightAmbient takes real dayR, real dayG, real dayB, real nightR, real nightG, real nightB returns nothing
        call WarVK_Command("set-day-night-ambient|" + R2S(dayR) + "|" + R2S(dayG) + "|" + R2S(dayB) + "|" + R2S(nightR) + "|" + R2S(nightG) + "|" + R2S(nightB))
    endfunction
endlibrary
