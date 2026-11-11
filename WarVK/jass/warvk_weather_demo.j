// 示例需先包含warvk_constant.j与warvk_api.j。手动调用WarVKWeatherDemoStart。
// 0/30/60/90秒仅为示例，作者应按选定音乐录音重新标记乐句。
// 开始音乐时调用Start；暂停音乐时调用Pause；重新定位时调用Seek。
globals
    integer wvkWeatherDemoId = 0
endglobals

function WarVKWeatherDemoCheck takes nothing returns boolean
    if WarVKGetLastErrorCode() == WARVK_ERROR_NONE then
        return true
    endif
    call BJDebugMsg("Weather setup failed: " + WarVKGetLastError())
    call WarVKDestroyWeather(wvkWeatherDemoId)
    set wvkWeatherDemoId = 0
    return false
endfunction

function WarVKWeatherDemoStart takes nothing returns nothing
    if wvkWeatherDemoId > 0 and WarVKIsWeatherAlive(wvkWeatherDemoId) then
        call WarVKDestroyWeather(wvkWeatherDemoId)
    endif
    set wvkWeatherDemoId = WarVKCreateSnowWeather(20261225)
    if wvkWeatherDemoId <= 0 then
        call BJDebugMsg("Weather creation failed: " + WarVKGetLastError())
        return
    endif
    call WarVKSetWeatherRegion(wvkWeatherDemoId, -4096.0, -4096.0, 4096.0, 4096.0, 0.0, 1800.0, 0.15)
    if not WarVKWeatherDemoCheck() then
        return
    endif
    call WarVKAddSnowWeatherKeyframe(wvkWeatherDemoId, 0.0, 0.10, 20.0, 40.0, 0.10, 0.05, 0.00)
    if not WarVKWeatherDemoCheck() then
        return
    endif
    call WarVKAddSnowWeatherKeyframe(wvkWeatherDemoId, 30.0, 0.40, 30.0, 160.0, 0.35, 0.25, 0.20)
    if not WarVKWeatherDemoCheck() then
        return
    endif
    call WarVKAddSnowWeatherKeyframe(wvkWeatherDemoId, 60.0, 0.90, 40.0, 420.0, 0.80, 0.65, 0.75)
    if not WarVKWeatherDemoCheck() then
        return
    endif
    call WarVKAddSnowWeatherKeyframe(wvkWeatherDemoId, 90.0, 0.25, 45.0, 80.0, 0.20, 0.15, 0.10)
    if not WarVKWeatherDemoCheck() then
        return
    endif
    call WarVKPlayWeatherTimeline(wvkWeatherDemoId, 0.0)
    if not WarVKWeatherDemoCheck() then
        return
    endif
endfunction

function WarVKWeatherDemoPause takes boolean paused returns nothing
    if WarVKIsWeatherAlive(wvkWeatherDemoId) then
        call WarVKSetWeatherPaused(wvkWeatherDemoId, paused)
    endif
endfunction

function WarVKWeatherDemoSeek takes real seconds returns nothing
    if WarVKIsWeatherAlive(wvkWeatherDemoId) then
        call WarVKSetWeatherTime(wvkWeatherDemoId, seconds)
    endif
endfunction

function WarVKWeatherDemoStop takes nothing returns nothing
    if wvkWeatherDemoId > 0 and WarVKIsWeatherAlive(wvkWeatherDemoId) then
        call WarVKDestroyWeather(wvkWeatherDemoId)
    endif
    set wvkWeatherDemoId = 0
endfunction
