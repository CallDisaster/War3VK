globals
    integer wsStage = 0
    integer wsChecks = 0
    integer wsErrors = 0
    integer wsId = 0
    integer wsCycle = -1
    integer wsOldId = 0
    integer wsPostfxRefusalCode = 0
    boolean wsOldRejected = false
    boolean wsWeatherEnabled = false
    boolean wsSunEnabled = true
    boolean wsOrdinaryEnabled = true
    boolean wsPlayed = false
    real wsStartQuery = 0.0
    string wsFailures = ""
endglobals

function WeatherStressCheck takes boolean passed, string label returns nothing
    set wsChecks = wsChecks + 1
    if not passed then
        set wsErrors = wsErrors + 1
        set wsFailures = wsFailures + label + ","
    endif
endfunction

function WeatherStressError takes string label returns nothing
    call WeatherStressCheck(WarVKGetLastErrorCode() == 0, label)
    call WarVKClearError()
endfunction

function WeatherStressOrdinary takes nothing returns nothing
    call WarVKSetCelestialMotionEnabled(false)
    call WeatherStressError("ordinary-celestial")
    call WarVKSetLightingClockTime(12.0)
    call WeatherStressError("ordinary-clock")
    call WarVKSetSunColorIntensity(1.0, 0.95, 0.85, 1.0)
    call WeatherStressError("ordinary-sun-color")
    call WarVKSetSunEnabled(wsSunEnabled)
    call WeatherStressError("ordinary-sun-enabled")
    call WarVKSetPostfxEnabled(wsStage < 4)
    // This existing catalog family is explicitly unimplemented (0x40).
    // Check the actual refusal, never pretend this switches the owner.
    set wsPostfxRefusalCode = WarVKGetLastErrorCode()
    call WeatherStressCheck(wsPostfxRefusalCode == 18, "ordinary-postfx-unsupported-refused")
    call WarVKClearError()
    call WarVKSetVolumetricEnabled(wsOrdinaryEnabled)
    call WeatherStressError("ordinary-volume-enabled")
    call WarVKSetGlobalVolumetricMediumEnabled(true)
    call WeatherStressError("ordinary-global-medium")
    call WarVKSetVolumetricDensity(0.12)
    call WeatherStressError("ordinary-density")
    // High is 2. Weather uses its independent fixed Medium owner.
    call WarVKSetVolumetricBackend(2)
    call WeatherStressError("ordinary-backend")
endfunction

function WeatherStressCreate takes nothing returns nothing
    set wsId = WarVKCreateSnowWeather(23463)
    call WeatherStressError("create")
    call WeatherStressCheck(wsId > 0 and WarVKIsWeatherAlive(wsId), "create-alive")
    call WeatherStressError("create-alive-query")
    call WarVKSetWeatherRegion(wsId, -3500.0, -3500.0, 3500.0, 3500.0, 0.0, 2200.0, 0.1)
    call WeatherStressError("create-region")
    call WarVKSetSnowWeatherTarget(wsId, 0.65, 30.0, 160.0, 0.4, 0.45, 0.5, 0.0)
    call WeatherStressError("create-target")
    call WarVKClearWeatherKeyframes(wsId)
    call WeatherStressError("create-clear-keyframes")
    call WarVKAddSnowWeatherKeyframe(wsId, 0.0, 0.65, 30.0, 160.0, 0.4, 0.45, 0.5)
    call WeatherStressError("create-keyframe-zero")
    call WarVKAddSnowWeatherKeyframe(wsId, 600.0, 0.65, 30.0, 160.0, 0.4, 0.45, 0.5)
    call WeatherStressError("create-keyframe-tail")
    call WarVKSetWeatherEnabled(wsId, false)
    call WeatherStressError("create-configuration")
endfunction

function WeatherStressPlay takes nothing returns nothing
    call WarVKPlayWeatherTimeline(wsId, 0.0)
    call WeatherStressError("play")
    set wsStartQuery = WarVKGetWeatherTime(wsId)
    call WeatherStressError("play-query")
    call WeatherStressCheck(wsStartQuery >= 0.0 and wsStartQuery < 0.08, "play-query-near-zero")
    set wsPlayed = true
    set wsWeatherEnabled = true
endfunction

function WeatherStressRefuseOldWrite takes nothing returns nothing
    // Valid but deliberately different params must be refused because the id
    // is stale, not because the params themselves are invalid.
    call WarVKSetSnowWeatherTarget(wsOldId, 0.2, 200.0, 20.0, 0.1, 0.1, 0.1, 0.0)
    set wsOldRejected = WarVKGetLastErrorCode() != 0
    call WeatherStressCheck(wsOldRejected, "old-id-write-refused")
    call WarVKClearError()
endfunction

function WeatherStressReceipt takes nothing returns nothing
    local real seconds = 0.0
    local boolean alive = false
    if wsId > 0 then
        set alive = WarVKIsWeatherAlive(wsId)
        call WeatherStressError("receipt-alive-query")
        if alive then
            set seconds = WarVKGetWeatherTime(wsId)
        endif
    endif
    call WeatherStressError("receipt-query")
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("WEATHER_STRESS_STAGE=" + I2S(wsStage) + ";checks=" + I2S(wsChecks) + ";errors=" + I2S(wsErrors) + ";id=" + I2S(wsId) + ";time=" + R2S(seconds) + ";cycle=" + I2S(wsCycle) + ";weather=" + WarVKBoolToken(wsWeatherEnabled) + ";sun=" + WarVKBoolToken(wsSunEnabled) + ";ordinary=" + WarVKBoolToken(wsOrdinaryEnabled) + ";played=" + WarVKBoolToken(wsPlayed) + ";query=" + R2S(wsStartQuery) + ";oldId=" + I2S(wsOldId) + ";oldRejected=" + WarVKBoolToken(wsOldRejected) + ";postfxRefusalCode=" + I2S(wsPostfxRefusalCode))
    call Preload("WEATHER_STRESS_FAILURES=" + wsFailures)
    call PreloadGenEnd("WarVK\\Temp\\weather_stress_stage_" + I2S(wsStage) + ".txt")
endfunction

function WeatherStressTick takes nothing returns nothing
    local timer nextTimer = null
    local real nextDelay = 30.0
    set wsOldId = 0
    set wsOldRejected = false
    set wsPlayed = false
    set wsStartQuery = 0.0
    if wsStage == 0 then
        call WeatherStressCheck(WarVKIsBridgeAvailable(), "bridge")
        call WeatherStressCheck(WarVKGetProtocolVersion() == 1, "protocol")
        call WeatherStressCheck(WarVKIsRuntimeReady(), "runtime-ready")
    endif
    if wsStage < 4 then
        set wsCycle = -1
        set wsSunEnabled = true
        set wsOrdinaryEnabled = wsStage != 2
    else
        set wsSunEnabled = false
        set wsOrdinaryEnabled = false
    endif
    call WeatherStressOrdinary()
    if wsStage == 0 then
        call WeatherStressCreate()
    elseif wsStage == 1 then
        call WeatherStressPlay()
    elseif wsStage >= 4 and wsStage <= 67 then
        set wsCycle = (wsStage - 4) / 2
        if ModuloInteger(wsStage - 4, 2) == 0 then
            // The eight rebuilds are cycles 3,7,...31. No parallel instance.
            if ModuloInteger(wsCycle + 1, 4) == 0 then
                set wsOldId = wsId
                call WarVKDestroyWeather(wsOldId)
                call WeatherStressError("rebuild-destroy")
                call WeatherStressCheck(not WarVKIsWeatherAlive(wsOldId), "old-id-dead")
                call WeatherStressError("rebuild-alive-query")
                call WeatherStressCreate()
                call WeatherStressRefuseOldWrite()
            endif
            call WeatherStressPlay()
            set nextDelay = 4.0
        else
            call WarVKSetWeatherEnabled(wsId, false)
            call WeatherStressError("cycle-disable")
            set wsStartQuery = WarVKGetWeatherTime(wsId)
            call WeatherStressError("cycle-query")
            call WeatherStressCheck(wsStartQuery >= 3.5 and wsStartQuery < 6.0, "cycle-time-progress")
            set wsWeatherEnabled = false
            set nextDelay = 2.0
        endif
    elseif wsStage == 68 then
        set wsCycle = 32
        set wsOldId = wsId
        call WarVKDestroyWeather(wsId)
        call WeatherStressError("tail-destroy")
        call WeatherStressCheck(not WarVKIsWeatherAlive(wsOldId), "tail-old-id-dead")
        call WeatherStressError("tail-alive-query")
        call WeatherStressRefuseOldWrite()
        set wsId = 0
        set wsWeatherEnabled = false
        set nextDelay = 60.0
    elseif wsStage == 69 then
        set wsCycle = 32
        call WeatherStressReceipt()
        call DestroyTimer(GetExpiredTimer())
        return
    endif
    call WeatherStressReceipt()
    set wsStage = wsStage + 1
    set nextTimer = CreateTimer()
    call TimerStart(nextTimer, nextDelay, false, function WeatherStressTick)
    call DestroyTimer(GetExpiredTimer())
    set nextTimer = null
endfunction

function config takes nothing returns nothing
    call SetMapName("WarVK weather coexistence stress 20261010")
    call SetMapDescription("Owned isolated 32-cycle weather fixture")
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
    set u = CreateUnit(Player(0), 'htow', 0.0, 250.0, 270.0)
    call SetUnitInvulnerable(u, true)
    set u = CreateUnit(Player(0), 'hbar', -500.0, 200.0, 270.0)
    call SetUnitInvulnerable(u, true)
    set u = CreateUnit(Player(0), 'hfoo', -200.0, -100.0, 0.0)
    call SetUnitInvulnerable(u, true)
    call SetCameraPosition(0.0, 0.0)
    call SetCameraField(CAMERA_FIELD_TARGET_DISTANCE, 1800.0, 0.0)
    call SetCameraField(CAMERA_FIELD_ANGLE_OF_ATTACK, 305.0, 0.0)
    call TimerStart(CreateTimer(), 25.0, false, function WeatherStressTick)
    call RemoveRect(bounds)
    set u = null
    set bounds = null
endfunction
