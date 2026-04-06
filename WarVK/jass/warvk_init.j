#pragma once
// WarVK JASS 初始化入口

library WarVKInit
    globals
        private boolean WARVK_INITIALIZED = false
    endglobals

    function WarVK_Init takes nothing returns nothing
        if (WARVK_INITIALIZED) then
            return
        endif
        set WARVK_INITIALIZED = true

        // 触发一次空调用，确保 JAPI 注入已生效
        call ExecuteFunc("DoNothing")
    endfunction
endlibrary

#include "API/warvk_render.j"
