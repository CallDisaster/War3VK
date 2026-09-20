globals
    integer v122NativeStage = 0
    integer v122NativeErrors = 0
    string v122NativeModel = "war3mapImported\\TorchHuman.mdx"
    string v122NativeStock = "Doodads\\LordaeronSummer\\Props\\TorchHumanOmni\\TorchHumanOmni.mdx"
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
    call Preload("V122_NATIVE_STAGE="+I2S(v122NativeStage)+";errors="+I2S(v122NativeErrors)+";author=0;")
    call Preload("IMPORTED="+I2S(WarVKGetModelPointLightCount(v122NativeModel))+";STOCK="+I2S(WarVKGetModelPointLightCount(v122NativeStock)))
    call PreloadGenEnd("WarVK\\Temp\\@NATIVE_RECEIPT@_stage_"+I2S(v122NativeStage)+".txt")
endfunction

function V122NativeTick takes nothing returns nothing
    if v122NativeStage == 0 then
        call WarVKSetSunEnabled(false)
        call WarVKSetModelPointLightsEnabled(v122NativeModel,true,true)
        call V122NativeCheck(WarVKGetLastErrorCode()==0)
        call WarVKSetModelPointLightsEnabled(v122NativeStock,true,true)
        call V122NativeCheck(WarVKGetLastErrorCode()==0)
        call V122NativeCheck(WarVKIsModelPointLightRegistered(v122NativeModel))
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeModel)>=1)
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeStock)>=1)
        set v122NativeCasters[0]=CreateUnit(Player(0),'hfoo',295.0,210.0,90.0)
        set v122NativeCasters[1]=CreateUnit(Player(0),'hfoo',-60.0,210.0,270.0)
        call SetUnitInvulnerable(v122NativeCasters[0],true)
        call SetUnitInvulnerable(v122NativeCasters[1],true)
        // Actual in-game selection/portrait state, no OS/global input.
        call SelectUnitSingle(v122NativeCasters[0])
    elseif v122NativeStage == 1 then
        call WarVKSetModelPointLightsEnabled(v122NativeModel,true,false)
        call WarVKSetModelPointLightsEnabled(v122NativeStock,true,false)
        call V122NativeCheck(WarVKGetLastErrorCode()==0)
        call SelectUnitSingle(v122NativeCasters[0])
    elseif v122NativeStage == 2 then
        call WarVKSetModelPointLightsEnabled(v122NativeModel,true,true)
        call WarVKSetModelPointLightsEnabled(v122NativeStock,true,true)
        call V122NativeCheck(WarVKGetLastErrorCode()==0)
        call SelectUnitSingle(v122NativeCasters[1])
    elseif v122NativeStage == 3 then
        // Keep the mixed scene and both lights active for the final screenshot.
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeModel)>=1)
        call V122NativeCheck(WarVKGetModelPointLightCount(v122NativeStock)>=1)
        call SelectUnitSingle(v122NativeCasters[0])
        call PauseTimer(GetExpiredTimer())
        call DestroyTimer(GetExpiredTimer())
    endif
    call V122NativeReceipt()
    set v122NativeStage=v122NativeStage+1
endfunction

function V122NativeStart takes nothing returns nothing
    call V122NativeTick()
    call TimerStart(CreateTimer(),25.0,true,function V122NativeTick)
endfunction
