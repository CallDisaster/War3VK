#pragma once
#include "warvk_constant.j"

library WarVKBridge
    globals
        private boolean WARVK_READY = false
        private string WARVK_BRIDGE_UNAVAILABLE_ERROR = "WarVK proxy d3d9.dll is not active; install it before starting Warcraft III"
    endglobals

    function WarVK_BoolArg takes boolean value returns string
        if (value) then
            return "1"
        endif
        return "0"
    endfunction

    function WarVK_QueryIntegerRaw takes string payload returns integer
        return GetLocalizedHotkey(WARVK_BRIDGE_PREFIX + payload)
    endfunction

    function WarVK_QueryStringRaw takes string payload returns string
        return GetLocalizedString(WARVK_BRIDGE_PREFIX + payload)
    endfunction

    function WarVK_RefreshLoaded takes nothing returns boolean
        if (WARVK_READY) then
            return true
        endif
        set WARVK_READY = (WarVK_QueryIntegerRaw("ping") == 1)
        return WARVK_READY
    endfunction

    function WarVK_IsLoaded takes nothing returns boolean
        return WarVK_RefreshLoaded()
    endfunction

    function WarVK_CommandRaw takes string payload returns nothing
        if (WarVK_RefreshLoaded()) then
            call Preloader(WARVK_BRIDGE_PREFIX + payload)
        endif
    endfunction

    function WarVK_Command takes string payload returns nothing
        if (WarVK_RefreshLoaded()) then
            call Preloader(WARVK_BRIDGE_PREFIX + "cmd:" + payload)
        endif
    endfunction

    function WarVK_QueryInteger takes string payload returns integer
        if (WarVK_RefreshLoaded()) then
            return GetLocalizedHotkey(WARVK_BRIDGE_PREFIX + "cmd:" + payload)
        endif
        return 0
    endfunction

    function WarVK_QueryString takes string payload returns string
        if (WarVK_RefreshLoaded()) then
            return GetLocalizedString(WARVK_BRIDGE_PREFIX + "cmd:" + payload)
        endif
        return ""
    endfunction

    function WarVK_Log takes string message returns nothing
        call WarVK_CommandRaw("log:" + message)
    endfunction

    function WarVK_ShowRuntimeStatus takes nothing returns nothing
        if (WarVK_RefreshLoaded()) then
            call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, "[WarVK] runtime ready: " + WarVK_QueryStringRaw("version"))
            return
        endif

        call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, "[WarVK] runtime unavailable: " + WARVK_BRIDGE_UNAVAILABLE_ERROR)
    endfunction

    function WarVK_GetVersionString takes nothing returns string
        if (WarVK_RefreshLoaded()) then
            return WarVK_QueryStringRaw("version")
        endif
        return ""
    endfunction

    function WarVK_GetLastErrorString takes nothing returns string
        if (WarVK_RefreshLoaded()) then
            return WarVK_QueryStringRaw("last-error")
        endif
        return WARVK_BRIDGE_UNAVAILABLE_ERROR
    endfunction

    function WarVK_GetStatsString takes nothing returns string
        if (WarVK_RefreshLoaded()) then
            return WarVK_QueryStringRaw("stats")
        endif
        return ""
    endfunction

    // Some trigger metadata generators strip underscores from custom function
    // names while saving a map. Keep no-underscore aliases so both handwritten
    // JASS and GUI-generated calls compile.
    function WarVKIsLoaded takes nothing returns boolean
        return WarVK_IsLoaded()
    endfunction

    function WarVKShowRuntimeStatus takes nothing returns nothing
        call WarVK_ShowRuntimeStatus()
    endfunction

    function WarVKGetLastErrorString takes nothing returns string
        return WarVK_GetLastErrorString()
    endfunction

    function WarVKGetVersionString takes nothing returns string
        return WarVK_GetVersionString()
    endfunction

    function WarVKGetStatsString takes nothing returns string
        return WarVK_GetStatsString()
    endfunction

    function WarVKLog takes string message returns nothing
        call WarVK_Log(message)
    endfunction
endlibrary
