globals
    integer v122NativeStage = 0
    effect array v122NativeEffects
    unit array v122NativeCasters
endglobals

// Real stock model producer: intentionally NO WarVKCreatePointLight call.
function V122NativeReceipt takes nothing returns nothing
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("V122_NATIVE_STAGE=" + I2S(v122NativeStage))
    call PreloadGenEnd("WarVK\\Temp\\@NATIVE_RECEIPT@_stage_" + I2S(v122NativeStage) + ".txt")
endfunction

function V122NativeTick takes nothing returns nothing
    local real x = GetCameraTargetPositionX()
    local real y = GetCameraTargetPositionY()
    if v122NativeStage == 0 then
        set v122NativeEffects[0] = AddSpecialEffect("Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman.mdx", x - 200.0, y)
        set v122NativeEffects[1] = AddSpecialEffect("Doodads\\LordaeronSummer\\Props\\brazierOmni\\brazierOmni.mdx", x + 200.0, y)
        set v122NativeCasters[0] = CreateUnit(Player(0), 'hfoo', x - 130.0, y + 30.0, 90.0)
        set v122NativeCasters[1] = CreateUnit(Player(0), 'hfoo', x + 130.0, y + 30.0, 90.0)
        call PauseUnit(v122NativeCasters[0], true)
        call PauseUnit(v122NativeCasters[1], true)
        call SetTimeOfDay(0.0)
        call SuspendTimeOfDay(true)
    elseif v122NativeStage == 1 then
        call DestroyEffect(v122NativeEffects[0])
        set v122NativeEffects[0] = null
    elseif v122NativeStage == 2 then
        call DestroyEffect(v122NativeEffects[1])
        set v122NativeEffects[1] = null
    elseif v122NativeStage == 3 then
        call RemoveUnit(v122NativeCasters[0])
        call RemoveUnit(v122NativeCasters[1])
        set v122NativeCasters[0] = null
        set v122NativeCasters[1] = null
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
