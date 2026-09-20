globals
    integer v122NativeStage = 0
    integer v122NativeErrors = 0
    string v122NativeModel = "war3mapImported\\WarVKReview\\TorchHumanUser32059406.mdx"
    effect array v122NativeEffects
    unit array v122NativeCasters
endglobals

function V122NativeCheck takes boolean ok returns nothing
    if not ok then
        set v122NativeErrors = v122NativeErrors + 1
    endif
endfunction

function V122NativeReceipt takes nothing returns nothing
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("V122_NATIVE_STAGE=" + I2S(v122NativeStage) + ";errors=" + I2S(v122NativeErrors) + ";author=0;")
    call Preload("MODEL_COUNT=" + I2S(WarVKGetModelPointLightCount(v122NativeModel)) + ";REGISTERED=" + WarVKBoolToken(WarVKIsModelPointLightRegistered(v122NativeModel)))
    call PreloadGenEnd("WarVK\\Temp\\@NATIVE_RECEIPT@_stage_" + I2S(v122NativeStage) + ".txt")
endfunction

function V122NativeTick takes nothing returns nothing
    local real x = GetCameraTargetPositionX()
    local real y = GetCameraTargetPositionY()
    if v122NativeStage == 0 then
        set x = x + 1200.0
        set y = y - 1500.0
        call SetCameraPosition(x,y)
        call WarVKSetCelestialMotionEnabled(false)
        call WarVKSetSunEnabled(false)
        call WarVKSetCsmEnabled(true)
        call WarVKSetVolumetricEnabled(false)
        call V122NativeCheck(WarVKGetLastErrorCode() == 0)
        // Load BEFORE registration: the exact existing instance must be found.
        set v122NativeEffects[0] = AddSpecialEffect(v122NativeModel,x-200.0,y)
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeModel) == 0)
        call WarVKSetModelPointLightsEnabled(v122NativeModel,true,true)
        call V122NativeCheck(WarVKGetLastErrorCode() == 0)
        call V122NativeCheck(WarVKIsModelPointLightRegistered(v122NativeModel))
        call V122NativeCheck(WarVKGetModelPointLightCount("WAR3MAPIMPORTED/WARVKREVIEW/TORCHHUMANUSER32059406.MDL") == 1)
        set v122NativeCasters[0] = CreateUnit(Player(0),'hfoo',x-130.0,y+30.0,90.0)
        call PauseUnit(v122NativeCasters[0],true)
        call SetTimeOfDay(0.0)
        call SuspendTimeOfDay(true)
        // Invalid OS path must not become a rule or clear the valid one.
        call WarVKSetModelPointLightsEnabled("C:\\bad.mdx",true,true)
        call V122NativeCheck(WarVKGetLastErrorCode() != 0)
        call WarVKClearError()
    elseif v122NativeStage == 1 then
        // Same scene, native light remains; only its point shadow is disabled.
        call WarVKSetModelPointLightsEnabled(v122NativeModel,true,false)
        call V122NativeCheck(WarVKGetLastErrorCode() == 0)
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeModel) == 1)
    elseif v122NativeStage == 2 then
        call WarVKSetModelPointLightsEnabled(v122NativeModel,true,true)
        call V122NativeCheck(WarVKGetLastErrorCode() == 0)
        call RemoveUnit(v122NativeCasters[0])
        set v122NativeCasters[0] = null
        // Future instance, same resource but an independent lifetime/generation.
        set v122NativeEffects[1] = AddSpecialEffect(v122NativeModel,x+1200.0,y)
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeModel) == 2)
    elseif v122NativeStage == 3 then
        call DestroyEffect(v122NativeEffects[0])
        call DestroyEffect(v122NativeEffects[1])
        call WarVKSetModelPointLightsEnabled(v122NativeModel,false,false)
        call V122NativeCheck(WarVKGetLastErrorCode() == 0)
        call V122NativeCheck(not WarVKIsModelPointLightRegistered(v122NativeModel))
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeModel) == 0)
        call PauseTimer(GetExpiredTimer())
        call DestroyTimer(GetExpiredTimer())
    endif
    call V122NativeReceipt()
    set v122NativeStage = v122NativeStage + 1
endfunction

function V122NativeStart takes nothing returns nothing
    call V122NativeTick()
    call TimerStart(CreateTimer(),15.0,true,function V122NativeTick)
endfunction
