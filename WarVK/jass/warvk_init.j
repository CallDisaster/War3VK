#pragma once
// WarVK map-side entry.
//
// Include this file from the map library. It declares no custom natives and
// only probes the proxy runtime that must already be active at process start.

#include "warvk_bridge.j"
#include "warvk_api.j"

library WarVKInitLib initializer WarVK_Init
    globals
        private boolean WARVK_INITIALIZED = false
    endglobals

    function WarVK_Init takes nothing returns nothing
        if (WARVK_INITIALIZED) then
            return
        endif
        set WARVK_INITIALIZED = true
        // Public calls perform their own readiness check. Initialization never
        // attempts to load a DLL or start an AI/Lua loader.
    endfunction

    function WarVKInit takes nothing returns nothing
        call WarVK_Init()
    endfunction
endlibrary
