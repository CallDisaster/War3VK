globals
    integer wcStage = 0
    integer wcChecks = 0
    integer wcErrors = 0
    integer wcId = 0
    string wcFailures = ""
    real wcFrozenTime = 0.0
    string wcPlayRaw = ""
    string wcTinyRaw = ""
endglobals

function WeatherCanaryCheck takes boolean passed, string label returns nothing
    set wcChecks = wcChecks + 1
    if not passed then
        set wcErrors = wcErrors + 1
        set wcFailures = wcFailures + label + ","
    endif
endfunction

function WeatherCanaryError takes string label returns nothing
    call WeatherCanaryCheck(WarVKGetLastErrorCode() == 0, label)
    call WarVKClearError()
endfunction

function WeatherCanaryReceipt takes nothing returns nothing
    local real t = 0.0
    if wcId > 0 and WarVKIsWeatherAlive(wcId) then
        set t = WarVKGetWeatherTime(wcId)
    endif
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("WEATHER_CANARY_STAGE=" + I2S(wcStage) + ";checks=" + I2S(wcChecks) + ";errors=" + I2S(wcErrors) + ";id=" + I2S(wcId) + ";time=" + R2S(t))
    call Preload("WEATHER_CANARY_FAILURES=" + wcFailures)
    call Preload("WEATHER_QUERY_DIAGNOSTIC=scientific:" + R2S(S2R("9.73e-05")) + ";decimal:" + R2S(S2R("0.0000973")) + ";playRaw:" + wcPlayRaw)
    call Preload("WEATHER_TINY_QUERY_RAW=" + wcTinyRaw)
    call PreloadGenEnd("WarVK\\Temp\\weather_canary_stage_" + I2S(wcStage) + ".txt")
endfunction

function WeatherCanaryTick takes nothing returns nothing
    local integer features = 0
    local real t = 0.0
    if wcStage == 0 then
        call WeatherCanaryCheck(WarVKIsBridgeAvailable(), "bridge")
        call WeatherCanaryCheck(WarVKGetProtocolVersion() == 1, "protocol")
        call WeatherCanaryCheck(WarVKIsRuntimeReady(), "runtime-ready")
        set features = WarVKGetFeatureFlags()
        // FeatureWeather=0x20000. Integer division avoids a non-stock bit native.
        call WeatherCanaryCheck(ModuloInteger(features / 131072, 2) == 1, "weather-feature")
        call WarVKClearError()
        call WarVKSetCelestialMotionEnabled(false)
        call WarVKSetLightingClockTime(12.0)
        call WarVKSetSunEnabled(false)
        call WarVKSetVolumetricEnabled(false)
        call WarVKSetGlobalVolumetricMediumEnabled(false)
        call WarVKSetVolumetricDensity(0.0)
        call WeatherCanaryError("independent-weather-setup")
        set wcId = WarVKCreateSnowWeather(-1)
        call WeatherCanaryCheck(wcId > 0 and WarVKIsWeatherAlive(wcId), "create")
        call WeatherCanaryError("create-error")
        call WarVKSetWeatherRegion(wcId, -3500.0, -3500.0, 3500.0, 3500.0, 0.0, 2200.0, 0.10)
        call WeatherCanaryError("region")
        call WarVKSetSnowWeatherTarget(wcId, 0.35, 350.0, 40.0, 0.10, 0.0, 0.0, 0.0)
        call WeatherCanaryError("manual-target")
    elseif wcStage == 1 then
        // Held manual phases isolate the two independent render systems.
        call WarVKSetWeatherEnabled(wcId, true)
        call WeatherCanaryError("snow-only-enable")
    elseif wcStage == 2 then
        call WarVKSetSnowWeatherTarget(wcId, 0.0, 350.0, 40.0, 0.10, 0.65, 0.0, 0.0)
        call WeatherCanaryError("haze-only-target")
    elseif wcStage == 3 then
        call WarVKSetSnowWeatherTarget(wcId, 0.0, 90.0, 100.0, 0.20, 0.0, 0.80, 0.0)
        call WeatherCanaryError("low-snow-only-target")
    elseif wcStage == 4 then
        call WarVKSetSnowWeatherTarget(wcId, 1.0, 10.0, 420.0, 0.80, 0.75, 0.80, 0.0)
        call WeatherCanaryError("full-target")
    elseif wcStage == 5 then
        call WarVKSetWeatherPaused(wcId, true)
        set wcFrozenTime = WarVKGetWeatherTime(wcId)
        call WeatherCanaryError("pause")
    elseif wcStage == 6 then
        set t = WarVKGetWeatherTime(wcId)
        call WeatherCanaryCheck(RAbsBJ(t - wcFrozenTime) < 0.08, "paused-time-stable")
        // Use the public textual protocol directly: stock R2S itself only
        // writes three decimals and cannot serialize this tiny test input.
        call Preloader("warvk:v1;weather.setTime;d:" + I2S(wcId) + ";r:0.0000973")
        call WeatherCanaryError("tiny-time-setup")
        set wcTinyRaw = GetLocalizedString("warvk:v1;weather.getTime;d:" + I2S(wcId))
        call WeatherCanaryCheck(RAbsBJ(S2R(wcTinyRaw) - 0.0000973) < 0.000001, "tiny-raw-query")
        call WeatherCanaryCheck(RAbsBJ(WarVKGetWeatherTime(wcId) - 0.0000973) < 0.000001, "tiny-wrapper-query")
        call WarVKSetWeatherTime(wcId, 2.0)
        call WeatherCanaryError("seek-paused")
        call WeatherCanaryCheck(RAbsBJ(WarVKGetWeatherTime(wcId) - 2.0) < 0.08, "seek-time")
    elseif wcStage == 7 then
        call WarVKSetWeatherPaused(wcId, false)
        call WeatherCanaryError("resume")
    elseif wcStage == 8 then
        call WeatherCanaryCheck(WarVKGetWeatherTime(wcId) >= 6.0, "resume-progress")
        call WarVKSetWeatherEnabled(wcId, false)
        set wcFrozenTime = WarVKGetWeatherTime(wcId)
        call WeatherCanaryError("disable")
    elseif wcStage == 9 then
        set t = WarVKGetWeatherTime(wcId)
        call WeatherCanaryCheck(RAbsBJ(t - wcFrozenTime) < 0.08, "disabled-time-stable")
        call WarVKSetWeatherEnabled(wcId, true)
        call WeatherCanaryError("reenable")
    elseif wcStage == 10 then
        call WeatherCanaryCheck(WarVKGetWeatherTime(wcId) >= wcFrozenTime + 4.0, "reenable-progress")
        call WarVKSetWeatherEnabled(wcId, false)
        call WarVKClearWeatherKeyframes(wcId)
        call WeatherCanaryError("disabled-clear")
        call WarVKAddSnowWeatherKeyframe(wcId, 0.0, 0.15, 350.0, 40.0, 0.10, 0.0, 0.0)
        call WarVKAddSnowWeatherKeyframe(wcId, 6.0, 1.0, 10.0, 420.0, 0.80, 0.75, 0.80)
        call WarVKAddSnowWeatherKeyframe(wcId, 60.0, 1.0, 10.0, 420.0, 0.80, 0.75, 0.80)
        call WeatherCanaryError("disabled-replacement-keyframes")
    elseif wcStage == 11 then
        call WarVKPlayWeatherTimeline(wcId, 0.0)
        call WeatherCanaryError("replacement-play")
        set wcPlayRaw = GetLocalizedString("warvk:v1;weather.getTime;d:" + I2S(wcId))
        call WeatherCanaryCheck(S2R(wcPlayRaw) >= 0.0 and S2R(wcPlayRaw) < 0.08, "play-immediate-query")
        call WeatherCanaryCheck(WarVKGetWeatherTime(wcId) < 0.08, "play-wrapper-query")
    elseif wcStage == 12 then
        set t = WarVKGetWeatherTime(wcId)
        call WeatherCanaryCheck(t >= 6.0 and t < 16.0, "timeline-progress")
    elseif wcStage == 13 then
        call WarVKDestroyWeather(wcId)
        call WeatherCanaryError("destroy")
        call WeatherCanaryCheck(not WarVKIsWeatherAlive(wcId), "destroy-stale")
    elseif wcStage == 14 then
        call WeatherCanaryCheck(not WarVKIsWeatherAlive(wcId), "destroy-stays-stale")
        call WeatherCanaryReceipt()
        call PauseTimer(GetExpiredTimer())
        call DestroyTimer(GetExpiredTimer())
        return
    endif
    call WeatherCanaryReceipt()
    set wcStage = wcStage + 1
endfunction

function WeatherCanaryStart takes nothing returns nothing
    call WeatherCanaryTick()
    call TimerStart(CreateTimer(), 8.0, true, function WeatherCanaryTick)
endfunction

function config takes nothing returns nothing
    call SetMapName("WarVK weather canary 20261010")
    call SetMapDescription("Isolated visual weather/API fixture")
    call SetPlayers(1)
    call SetTeams(1)
    call SetGamePlacement(MAP_PLACEMENT_USE_MAP_SETTINGS)
    call DefineStartLocation(0, 0.0, 0.0)
    call SetPlayerStartLocation(Player(0), 0)
    call SetPlayerColor(Player(0), PLAYER_COLOR_RED)
    call SetPlayerRacePreference(Player(0), RACE_PREF_HUMAN)
    call SetPlayerRaceSelectable(Player(0), false)
    call SetPlayerController(Player(0), MAP_CONTROL_USER)
    call SetPlayerTeam(Player(0), 0)
    call SetPlayerSlotAvailable(Player(0), MAP_CONTROL_USER)
endfunction

function main takes nothing returns nothing
    local rect bounds = GetWorldBounds()
    local real left = GetRectMinX(bounds) + 128.0
    local real right = GetRectMaxX(bounds) - 128.0
    local real bottom = GetRectMinY(bounds) + 128.0
    local real top = GetRectMaxY(bounds) - 128.0
    local unit u = null
    call SetCameraBounds(left, bottom, right, top, left, top, right, bottom)
    call SetDayNightModels("Environment\\DNC\\DNCLordaeron\\DNCLordaeronTerrain\\DNCLordaeronTerrain.mdl", "Environment\\DNC\\DNCLordaeron\\DNCLordaeronUnit\\DNCLordaeronUnit.mdl")
    call NewSoundEnvironment("Default")
    call InitBlizzard()
    call FogEnable(false)
    call FogMaskEnable(false)
    call SetFloatGameState(GAME_STATE_TIME_OF_DAY, 12.0)
    call SuspendTimeOfDay(true)
    call SetPlayerState(Player(0), PLAYER_STATE_RESOURCE_GOLD, 1000)
    set u = CreateUnit(Player(0), 'htow', 0.0, 250.0, 270.0)
    call SetUnitInvulnerable(u, true)
    set u = CreateUnit(Player(0), 'hbar', -500.0, 200.0, 270.0)
    call SetUnitInvulnerable(u, true)
    set u = CreateUnit(Player(0), 'hfoo', -200.0, -100.0, 0.0)
    call SetUnitInvulnerable(u, true)
    set u = CreateUnit(Player(0), 'hfoo', 200.0, -100.0, 180.0)
    call SetUnitInvulnerable(u, true)
    call SetCameraPosition(0.0, 0.0)
    call SetCameraField(CAMERA_FIELD_TARGET_DISTANCE, 1800.0, 0.0)
    call SetCameraField(CAMERA_FIELD_ANGLE_OF_ATTACK, 305.0, 0.0)
    call TimerStart(CreateTimer(), 25.0, false, function WeatherCanaryStart)
    call RemoveRect(bounds)
    set bounds = null
    set u = null
endfunction
