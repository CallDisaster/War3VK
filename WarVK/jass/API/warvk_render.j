#pragma once
#include "../warvk_constant.j"

#ifdef WARVK_USE_NATIVE
// WarVK 渲染 JASS API (native 需要在顶层声明)
// 时间/版本
native DXVK_GetGameTimeSeconds takes nothing returns real
native DXVK_GetTimeOfDay takes nothing returns real
native DXVK_GetGameVersion takes nothing returns integer

// 点光源
native AddPointLight takes real x, real y, real z, real range, real r, real g, real b, real intensity, real shadowIntensity returns integer
native UpdatePointLight takes integer id, real x, real y, real z, real range, real r, real g, real b, real intensity returns boolean
native RemovePointLight takes integer id returns boolean

// 描边句柄
native DXVK_OutlineAddHandle takes integer handleId returns nothing
native DXVK_OutlineRemoveHandle takes integer handleId returns nothing
native DXVK_OutlineClearHandles takes nothing returns nothing

// Bloom 句柄
native DXVK_BloomAddHandle takes integer handleId, real boost returns nothing
native DXVK_BloomRemoveHandle takes integer handleId returns nothing
native DXVK_BloomClearHandles takes nothing returns nothing
native DXVK_BloomGetBoost takes integer handleId returns real

// 光影/阴影
native DXVK_SetLightingEnabled takes boolean enabled returns nothing
native DXVK_SetSunDirection takes real x, real y, real z returns nothing
native DXVK_SetSunColor takes real r, real g, real b returns nothing
native DXVK_SetSunIntensity takes real intensity returns nothing
native DXVK_SetShadowEnabled takes boolean enabled returns nothing
native DXVK_SetShadowStrength takes real strength returns nothing
native DXVK_SetShadowBias takes real bias returns nothing
native DXVK_SetShadowPcfRadius takes real radius returns nothing
native DXVK_SetShadowDebugMode takes integer mode returns nothing
native DXVK_SetPointLightsEnabled takes boolean enabled returns nothing
native DXVK_SetPointShadowEnabled takes boolean enabled returns nothing

// 描边设置
native DXVK_SetOutlineEnabled takes boolean enabled returns nothing
native DXVK_SetOutlineWidth takes real widthPx returns nothing
native DXVK_SetOutlineColor takes real r, real g, real b, real a returns nothing
native DXVK_SetOutlineMode takes integer mode returns nothing
native DXVK_SetOutlineVisibility takes boolean showVisible, boolean showOccluded returns nothing

// 后处理
native DXVK_SetPostFxEnabled takes boolean enabled returns nothing
native DXVK_SetExposure takes real exposure returns nothing
native DXVK_SetBloomEnabled takes boolean enabled returns nothing
native DXVK_SetBloomParams takes real threshold, real softKnee, real intensity returns nothing
native DXVK_SetAcesEnabled takes boolean enabled returns nothing
native DXVK_SetSsaoEnabled takes boolean enabled returns nothing
native DXVK_SetSsaoParams takes real radiusPx, real strength, real bias, real power returns nothing
native DXVK_SetAaMode takes integer mode returns nothing
native DXVK_SetFxaaParams takes real subpix, real edgeThreshold, real edgeThresholdMin returns nothing
native DXVK_SetSmaaParams takes real threshold, integer search, integer diagSearch returns nothing

// 日夜
native DXVK_SetDayNightEnabled takes boolean enabled returns nothing
native DXVK_SetDayNightMinFactor takes real minFactor returns nothing
native DXVK_SetDayNightAmbient takes real dayR, real dayG, real dayB, real nightR, real nightG, real nightB returns nothing
#endif

// DEBUG
native DXVK_GetPluginVersionString takes nothing returns string
native DXVK_GetPluginVersion takes nothing returns integer
native TestIntReturn takes nothing returns integer
native TestFloatReturn takes nothing returns real
native TestStringReturn takes nothing returns string
native TestBooleanReturn takes nothing returns boolean

library WarVKRender
#ifndef WARVK_USE_NATIVE
    // 时间/版本
    function DXVK_GetGameTimeSeconds takes nothing returns real
        WarVK_JapiPlaceHolder 0.
    endfunction
    function DXVK_GetTimeOfDay takes nothing returns real
        WarVK_JapiPlaceHolder 0.
    endfunction
    function DXVK_GetGameVersion takes nothing returns integer
        WarVK_JapiPlaceHolder 0
    endfunction

    // 点光源
    function AddPointLight takes real x, real y, real z, real range, real r, real g, real b, real intensity, real shadowIntensity returns integer
        WarVK_JapiPlaceHolder 0
    endfunction
    function UpdatePointLight takes integer id, real x, real y, real z, real range, real r, real g, real b, real intensity returns boolean
        WarVK_JapiPlaceHolder false
    endfunction
    function RemovePointLight takes integer id returns boolean
        WarVK_JapiPlaceHolder false
    endfunction

    // 描边句柄
    function DXVK_OutlineAddHandle takes integer handleId returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_OutlineRemoveHandle takes integer handleId returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_OutlineClearHandles takes nothing returns nothing
        WarVK_JapiPlaceHolder
    endfunction

    // Bloom 句柄
    function DXVK_BloomAddHandle takes integer handleId, real boost returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_BloomRemoveHandle takes integer handleId returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_BloomClearHandles takes nothing returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_BloomGetBoost takes integer handleId returns real
        WarVK_JapiPlaceHolder 0.
    endfunction

    // 光影/阴影
    function DXVK_SetLightingEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetSunDirection takes real x, real y, real z returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetSunColor takes real r, real g, real b returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetSunIntensity takes real intensity returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetShadowEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetShadowStrength takes real strength returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetShadowBias takes real bias returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetShadowPcfRadius takes real radius returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetShadowDebugMode takes integer mode returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetPointLightsEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetPointShadowEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction

    // 描边设置
    function DXVK_SetOutlineEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetOutlineWidth takes real widthPx returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetOutlineColor takes real r, real g, real b, real a returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetOutlineMode takes integer mode returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetOutlineVisibility takes boolean showVisible, boolean showOccluded returns nothing
        WarVK_JapiPlaceHolder
    endfunction

    // 后处理
    function DXVK_SetPostFxEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetExposure takes real exposure returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetBloomEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetBloomParams takes real threshold, real softKnee, real intensity returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetAcesEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetSsaoEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetSsaoParams takes real radiusPx, real strength, real bias, real power returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetAaMode takes integer mode returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetFxaaParams takes real subpix, real edgeThreshold, real edgeThresholdMin returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetSmaaParams takes real threshold, integer search, integer diagSearch returns nothing
        WarVK_JapiPlaceHolder
    endfunction

    // 日夜
    function DXVK_SetDayNightEnabled takes boolean enabled returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetDayNightMinFactor takes real minFactor returns nothing
        WarVK_JapiPlaceHolder
    endfunction
    function DXVK_SetDayNightAmbient takes real dayR, real dayG, real dayB, real nightR, real nightG, real nightB returns nothing
        WarVK_JapiPlaceHolder
    endfunction
#endif
endlibrary
