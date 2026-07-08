#pragma once
// WarVK map-side entry.
//
// Include this file from the map library. It will not declare custom natives,
// so the map can pass native-table validation before WarVK is loaded.

#include "warvk_bridge.j"
#include "API/warvk_render.j"
#include "API/warvk_lightning.j"

library WarVKInitLib initializer WarVK_AutoInit
    globals
        private boolean WARVK_INITIALIZED = false
    endglobals

    function WarVK_Init takes nothing returns nothing
        if (WARVK_INITIALIZED) then
            return
        endif
        set WARVK_INITIALIZED = true
        call WarVK_Load()
    endfunction

    function WarVKInit takes nothing returns nothing
        call WarVK_Init()
    endfunction

    private function WarVK_AutoInitDelayed takes nothing returns nothing
        local timer t = GetExpiredTimer()
        call PauseTimer(t)
        call DestroyTimer(t)
        set t = null
        call WarVK_Init()
    endfunction

    private function WarVK_AutoInit takes nothing returns nothing
        local timer t = CreateTimer()
        call TimerStart(t, WARVK_AUTO_INIT_DELAY_SEC, false, function WarVK_AutoInitDelayed)
        set t = null
    endfunction
endlibrary
