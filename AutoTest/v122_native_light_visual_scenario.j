globals
    integer v122NativeStage = 0
    integer v122NativeAuthor = 0
    integer v122NativeErrors = 0
    effect array v122NativeEffects
    unit array v122NativeCasters
endglobals

function V122NativeReceipt takes nothing returns nothing
    local location probe = Location(GetCameraTargetPositionX() - 200.0, GetCameraTargetPositionY())
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("V122_NATIVE_STAGE=" + I2S(v122NativeStage) + ";errors=" + I2S(v122NativeErrors) + ";author=" + I2S(v122NativeAuthor))
    call Preload("GROUND_Z=" + R2S(GetLocationZ(probe)) + ";CAMERA_Z=" + R2S(GetCameraTargetPositionZ()))
    call PreloadGenEnd("WarVK\\Temp\\@NATIVE_RECEIPT@_stage_" + I2S(v122NativeStage) + ".txt")
    call RemoveLocation(probe)
    set probe = null
endfunction

function V122NativeTick takes nothing returns nothing
    local real x = GetCameraTargetPositionX()
    local real y = GetCameraTargetPositionY()
    if v122NativeStage == 0 then
        // Move away from the original hero/altar crowd. The copied source map
        // is unchanged; only this isolated fixture's camera/effects move.
        set x = x + 1200.0
        set y = y - 1500.0
        call SetCameraPosition(x, y)
        call WarVKSetCelestialMotionEnabled(false)
        call WarVKSetSunEnabled(false)
        call WarVKSetCsmEnabled(true)
        call WarVKSetVolumetricEnabled(false)
        if not WarVKIsBridgeAvailable() or WarVKGetLastErrorCode() != 0 then
            set v122NativeErrors = v122NativeErrors + 1
        endif
        set v122NativeEffects[0] = AddSpecialEffect("Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman.mdx", x - 200.0, y)
        set v122NativeEffects[1] = AddSpecialEffect("Doodads\\LordaeronSummer\\Props\\brazierOmni\\brazierOmni.mdx", x + 200.0, y)
        set v122NativeCasters[0] = CreateUnit(Player(0), 'hfoo', x - 130.0, y + 30.0, 90.0)
        set v122NativeCasters[1] = CreateUnit(Player(0), 'hfoo', x + 130.0, y + 30.0, 90.0)
        call PauseUnit(v122NativeCasters[0], true)
        call PauseUnit(v122NativeCasters[1], true)
        call SetTimeOfDay(0.0)
        call SuspendTimeOfDay(true)
    elseif v122NativeStage == 1 then
        // Light remains; only the actual occluder is removed.
        call RemoveUnit(v122NativeCasters[0])
        set v122NativeCasters[0] = null
    elseif v122NativeStage == 2 then
        // Separate authored channel, using finite normalized API colors.
        set v122NativeAuthor = WarVKCreatePointLight(x, y - 250.0, GetCameraTargetPositionZ() + 120.0, 300.0, 0.2, 0.4, 1.0, 0.3)
        call WarVKSetPointLightShadowEnabled(v122NativeAuthor, true)
        if v122NativeAuthor <= 0 or not WarVKIsPointLightAlive(v122NativeAuthor) or WarVKGetLastErrorCode() != 0 then
            set v122NativeErrors = v122NativeErrors + 1
        endif
    elseif v122NativeStage == 3 then
        call DestroyEffect(v122NativeEffects[0])
        call DestroyEffect(v122NativeEffects[1])
        call RemoveUnit(v122NativeCasters[1])
        call WarVKDestroyPointLight(v122NativeAuthor)
        if WarVKIsPointLightAlive(v122NativeAuthor) then
            set v122NativeErrors = v122NativeErrors + 1
        endif
        call PauseTimer(GetExpiredTimer())
        call DestroyTimer(GetExpiredTimer())
    endif
    call V122NativeReceipt()
    set v122NativeStage = v122NativeStage + 1
endfunction

function V122NativeStart takes nothing returns nothing
    call V122NativeTick()
    call TimerStart(CreateTimer(), 15.0, true, function V122NativeTick)
endfunction
