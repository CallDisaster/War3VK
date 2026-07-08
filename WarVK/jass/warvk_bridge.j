#pragma once
#include "warvk_constant.j"

library WarVKBridge
    globals
        private boolean WARVK_POLLING = false
        private boolean WARVK_READY = false
        private integer WARVK_LOAD_ATTEMPTS = 0
        private integer WARVK_LOAD_REQUESTS = 0
        private integer WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_EXTERNAL
        private string WARVK_LAST_LOAD_ROUTE = "none"
        private string WARVK_LAST_LOAD_ERROR = "WarVK bridge is not loaded"
        private timer WARVK_LOAD_TIMER = null
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

    function WarVK_GetLoaderMode takes nothing returns integer
        return WARVK_LOADER_MODE_DEFAULT
    endfunction

    function WarVK_GetLoaderModeString takes nothing returns string
        if (WARVK_LOADER_MODE_DEFAULT == WARVK_LOADER_MODE_YDWE_EXEC_LUA) then
            return "ydwe-exec-lua"
        endif
        if (WARVK_LOADER_MODE_DEFAULT == WARVK_LOADER_MODE_MEMORY_BRIDGE) then
            return "memory-bridge"
        endif
        if (WARVK_LOADER_MODE_DEFAULT == WARVK_LOADER_MODE_AI_BRIDGE) then
            return "ai-bridge"
        endif
        return "external"
    endfunction

    function WarVK_GetLastLoadRouteString takes nothing returns string
        return WARVK_LAST_LOAD_ROUTE
    endfunction

    function WarVK_GetLocalLoadErrorString takes nothing returns string
        return WARVK_LAST_LOAD_ERROR
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

    function WarVK_ShowLoaderStatus takes nothing returns nothing
        if (WarVK_RefreshLoaded()) then
            call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, "[WarVK] loaded: " + WarVK_QueryStringRaw("version"))
            return
        endif

        call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, "[WarVK] not loaded. mode=" + WarVK_GetLoaderModeString() + ", route=" + WARVK_LAST_LOAD_ROUTE + ", error=" + WARVK_LAST_LOAD_ERROR)
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
        return WARVK_LAST_LOAD_ERROR
    endfunction

    function WarVK_GetStatsString takes nothing returns string
        if (WarVK_RefreshLoaded()) then
            return WarVK_QueryStringRaw("stats")
        endif
        return ""
    endfunction

    function WarVK_RequestDllLoadByLua takes nothing returns boolean
#if WARVK_LOAD_USE_YDWE_EXEC_LUA
        set WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_YDWE_EXEC_LUA
        set WARVK_LAST_LOAD_ROUTE = WARVK_LOADER_EXEC_LUA_COMMAND
        set WARVK_LAST_LOAD_ERROR = "Requested YDWE Lua loader; waiting for WarVK.dll bridge"
        call Cheat(WARVK_LOADER_EXEC_LUA_COMMAND)
        return true
#endif
        set WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_YDWE_EXEC_LUA
        set WARVK_LAST_LOAD_ROUTE = "ydwe-exec-lua-disabled"
        set WARVK_LAST_LOAD_ERROR = "YDWE exec-lua loader is disabled in warvk_constant.j"
        return false
    endfunction

    function WarVK_RequestDllLoadByMemory takes nothing returns boolean
        set WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_MEMORY_BRIDGE
        set WARVK_LAST_LOAD_ROUTE = "clean-room-memory-bridge"
        set WARVK_LAST_LOAD_ERROR = "Non-Lua memory loader is not implemented; pure JASS cannot call LoadLibrary without a clean-room memory-call backend"
        return false
    endfunction

    function WarVK_RequestDllLoadByAi takes nothing returns boolean
#if WARVK_LOAD_USE_AI_BRIDGE
        local string scriptPath = WARVK_LOADER_AI_SCRIPT
        if (WARVK_LOAD_REQUESTS == 2) then
            set scriptPath = WARVK_LOADER_AI_SCRIPT_IMPORTED
        endif
        set WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_AI_BRIDGE
        set WARVK_LAST_LOAD_ROUTE = scriptPath
        set WARVK_LAST_LOAD_ERROR = "Requested WarVK AI bridge; waiting for WarVK.dll bridge"
        call StartCampaignAI(Player(PLAYER_NEUTRAL_AGGRESSIVE), scriptPath)
        return true
#endif
        set WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_AI_BRIDGE
        set WARVK_LAST_LOAD_ROUTE = "ai-bridge-disabled"
        set WARVK_LAST_LOAD_ERROR = "WarVK AI bridge is disabled in warvk_constant.j"
        return false
    endfunction

    function WarVK_RequestDllLoad takes nothing returns nothing
        if (WarVK_RefreshLoaded()) then
            return
        endif

        if (WARVK_LOAD_REQUESTS >= WARVK_LOADER_MAX_REQUESTS) then
            set WARVK_LAST_LOAD_ERROR = "WarVK load request limit reached; last route=" + WARVK_LAST_LOAD_ROUTE
            return
        endif

        set WARVK_LOAD_REQUESTS = WARVK_LOAD_REQUESTS + 1

        if (WARVK_LOADER_MODE_DEFAULT == WARVK_LOADER_MODE_AI_BRIDGE) then
            call WarVK_RequestDllLoadByAi()
            return
        endif

        if (WARVK_LOADER_MODE_DEFAULT == WARVK_LOADER_MODE_YDWE_EXEC_LUA) then
            call WarVK_RequestDllLoadByLua()
            return
        endif

        if (WARVK_LOADER_MODE_DEFAULT == WARVK_LOADER_MODE_MEMORY_BRIDGE) then
            call WarVK_RequestDllLoadByMemory()
            return
        endif

        set WARVK_LAST_LOAD_MODE = WARVK_LOADER_MODE_EXTERNAL
        set WARVK_LAST_LOAD_ROUTE = "external"
        set WARVK_LAST_LOAD_ERROR = "WarVK loader mode is external; load WarVK.dll by another launcher and keep polling"
    endfunction

    private function WarVK_OnLoadPoll takes nothing returns nothing
        set WARVK_LOAD_ATTEMPTS = WARVK_LOAD_ATTEMPTS + 1

        if (WarVK_RefreshLoaded()) then
            call PauseTimer(WARVK_LOAD_TIMER)
            call DestroyTimer(WARVK_LOAD_TIMER)
            set WARVK_LOAD_TIMER = null
            set WARVK_POLLING = false
            call WarVK_Log("jass-bridge-ready")
            call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 4.0, "[WarVK] bridge ready: " + WarVK_GetVersionString())
            return
        endif

        if (WARVK_LOAD_ATTEMPTS >= WARVK_LOAD_MAX_ATTEMPTS) then
            call PauseTimer(WARVK_LOAD_TIMER)
            call DestroyTimer(WARVK_LOAD_TIMER)
            set WARVK_LOAD_TIMER = null
            set WARVK_POLLING = false
            call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, "[WarVK] bridge not ready. mode=" + WarVK_GetLoaderModeString() + ", route=" + WARVK_LAST_LOAD_ROUTE + ", error=" + WARVK_LAST_LOAD_ERROR)
            return
        endif
    endfunction

    function WarVK_Load takes nothing returns boolean
        if (WarVK_RefreshLoaded()) then
            return true
        endif

        if (WARVK_POLLING) then
            return false
        endif

        set WARVK_POLLING = true
        set WARVK_LOAD_ATTEMPTS = 0
        set WARVK_LOAD_REQUESTS = 0
        call WarVK_RequestDllLoad()

        if (WARVK_LOAD_TIMER == null) then
            set WARVK_LOAD_TIMER = CreateTimer()
        endif
        call TimerStart(WARVK_LOAD_TIMER, WARVK_LOAD_RETRY_PERIOD, true, function WarVK_OnLoadPoll)
        return false
    endfunction

    // Some trigger metadata generators strip underscores from custom function
    // names while saving a map. Keep no-underscore aliases so both handwritten
    // JASS and GUI-generated calls compile.
    function WarVKLoad takes nothing returns boolean
        return WarVK_Load()
    endfunction

    function WarVKIsLoaded takes nothing returns boolean
        return WarVK_IsLoaded()
    endfunction

    function WarVKRequestDllLoadByLua takes nothing returns boolean
        return WarVK_RequestDllLoadByLua()
    endfunction

    function WarVKRequestDllLoadByMemory takes nothing returns boolean
        return WarVK_RequestDllLoadByMemory()
    endfunction

    function WarVKRequestDllLoadByAi takes nothing returns boolean
        return WarVK_RequestDllLoadByAi()
    endfunction

    function WarVKRequestDllLoad takes nothing returns nothing
        call WarVK_RequestDllLoad()
    endfunction

    function WarVKShowLoaderStatus takes nothing returns nothing
        call WarVK_ShowLoaderStatus()
    endfunction

    function WarVKGetLoaderMode takes nothing returns integer
        return WarVK_GetLoaderMode()
    endfunction

    function WarVKGetLoaderModeString takes nothing returns string
        return WarVK_GetLoaderModeString()
    endfunction

    function WarVKGetLastLoadRouteString takes nothing returns string
        return WarVK_GetLastLoadRouteString()
    endfunction

    function WarVKGetLocalLoadErrorString takes nothing returns string
        return WarVK_GetLocalLoadErrorString()
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
