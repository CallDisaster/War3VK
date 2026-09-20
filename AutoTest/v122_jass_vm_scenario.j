globals
    integer v122Stage = 0
    integer v122Checks = 0
    integer v122Errors = 0
    string v122Failures = ""
    integer v122Fog = 0
    integer array v122Slots
endglobals

function V122Check takes boolean condition, string label returns nothing
    set v122Checks = v122Checks + 1
    if not condition then
        set v122Errors = v122Errors + 1
        set v122Failures = v122Failures + label + ","
    endif
endfunction

function V122Error takes integer expected, string label returns nothing
    call V122Check(WarVKGetLastErrorCode() == expected, label)
    call WarVKClearError()
endfunction

function V122Receipt takes nothing returns nothing
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("V122_VM_STAGE=" + I2S(v122Stage) + ";checks=" + I2S(v122Checks) + ";errors=" + I2S(v122Errors))
    call Preload("V122_VM_FAILURES=" + v122Failures)
    call PreloadGenEnd("WarVK\\Temp\\@V122_RECEIPT_PREFIX@_stage_" + I2S(v122Stage) + ".txt")
endfunction

function V122Tick takes nothing returns nothing
    local real x = GetCameraTargetPositionX()
    local real y = GetCameraTargetPositionY()
    local real z = GetCameraTargetPositionZ() + 150.0
    local integer light = 0
    local integer program = 0
    local integer curve = 0
    local integer i = 0
    local integer excess = 0
    local real scalar = 0.0
    if v122Stage == 0 then
        call V122Check(WarVKIsBridgeAvailable(), "bridge")
        call V122Check(WarVKGetProtocolVersion() == 1, "protocol")
        call V122Check(StringLength(WarVKGetVersion()) > 0, "version-string")
        call V122Check(WarVKIsRuntimeReady(), "runtime-ready")
        call V122Check(WarVKGetFeatureFlags() > 0, "features")
        call WarVKClearError()
        call V122Check(WarVKGetLastErrorCode() == 0, "clear-error")
        call WarVKSetCelestialMotionEnabled(false)
        call V122Error(0, "celestial")
        call WarVKSetTimeColorGradingEnabled(false)
        call V122Error(0, "color-clock")
        call WarVKSetLightingClockTime(12.0)
        call V122Error(0, "held-clock")
        call WarVKSetSunDirection(-0.35, -0.8, -0.48)
        call V122Error(0, "sun-direction")
        call WarVKSetSunColorIntensity(1.0, 0.9, 0.7, 1.5)
        call V122Error(0, "sun-color")
        call WarVKSetSunEnabled(true)
        call V122Error(0, "sun-on")
        call WarVKSetCsmEnabled(true)
        call V122Error(0, "csm-on")
        call WarVKSetVolumetricEnabled(false)
        call V122Error(0, "volume-off")
        // The typed path must execute inside the real JASS VM, not fabricated
        // native arguments. Then force the documented string fallback below.
        call V122Check(WVKTypedReady(), "typed-handshake")
        set light = WarVKCreatePointLight(x, y, z, 400.0, 1.0, 0.5, 0.25, 0.7)
        call V122Check(light > 0 and WarVKIsPointLightAlive(light), "point-create")
        call WarVKSetPointLightEnabled(light, true)
        call V122Error(0, "point-enable")
        call WarVKSetPointLightPosition(light, x + 20.0, y, z)
        call V122Error(0, "typed-point-position")
        call WarVKSetPointLightColorIntensity(light, 0.5, 0.4, 0.3, 0.6)
        call V122Error(0, "typed-point-color")
        call WarVKSetPointLightRadius(light, 350.0)
        call V122Error(0, "typed-point-radius")
        set program = WarVKCompileMathProgram("2*t+1")
        call V122Check(program > 0 and WarVKIsMathProgramAlive(program), "math-compile")
        set curve = WarVKCreateCurve(program)
        call V122Check(curve > 0, "curve-create")
        set scalar = WarVKEvaluateMathReal(curve, 0.25, 0.0, 3)
        call V122Check(RAbsBJ(scalar - 1.5) < 0.0001, "typed-real-return")
        call V122Check(WarVKEvaluateMathInteger(curve, 0.25, 0.0, 3, 1) == 1, "typed-int-return")
        set wvkTypedState = -1
        call WarVKSetPointLightPosition(light, x, y, z)
        call V122Error(0, "string-point-position")
        call WarVKSetPointLightColorIntensity(light, 1.0, 0.5, 0.25, 0.7)
        call V122Error(0, "string-point-color")
        call WarVKSetPointLightRadius(light, 400.0)
        call V122Error(0, "string-point-radius")
        set scalar = WarVKEvaluateMathReal(curve, 0.25, 0.0, 3)
        call V122Check(RAbsBJ(scalar - 1.5) < 0.0001, "string-real-return")
        call V122Check(WarVKEvaluateMathInteger(curve, 0.25, 0.0, 3, 1) == 1, "string-int-return")
        set wvkTypedState = 1
        call WarVKDestroyCurve(curve)
        call V122Error(0, "curve-destroy")
        call WarVKDestroyMathProgram(program)
        call V122Check(not WarVKIsMathProgramAlive(program), "math-stale")
        call WarVKDestroyPointLight(light)
        call V122Check(not WarVKIsPointLightAlive(light), "point-stale")
    elseif v122Stage == 1 then
        call WarVKSetSunEnabled(false)
        call V122Error(0, "sun-off")
    elseif v122Stage == 2 then
        call WarVKSetSunEnabled(true)
        call V122Error(0, "sun-restore")
        call WarVKSetGlobalVolumetricMediumEnabled(false)
        call V122Error(0, "global-medium-off")
        call WarVKSetVolumetricDensity(0.0)
        call V122Error(0, "global-density-zero")
        call WarVKSetGlobalVolumetricFogEnabled(false)
        call V122Error(0, "global-height-fog-off")
        call WarVKSetVolumetricScattering(2.1, 0.96)
        call V122Error(0, "scattering")
        call WarVKSetVolumetricQuality(16, 1800.0)
        call V122Error(0, "quality")
        call WarVKSetVolumetricBackend(2)
        call V122Error(0, "high")
        call WarVKSetVolumetricEnabled(true)
        call V122Error(0, "volume-enable")
        set v122Fog = WarVKCreateSphereFogVolume(x, y, z, 650.0, 0.8, 0.2)
        call V122Check(v122Fog > 0 and WarVKIsFogVolumeAlive(v122Fog), "sphere-create")
    elseif v122Stage == 3 then
        call WarVKSetFogVolumeEnabled(v122Fog, false)
        call V122Error(0, "fog-disable")
    elseif v122Stage == 4 then
        call WarVKSetFogVolumeEnabled(v122Fog, true)
        call V122Error(0, "fog-enable")
        call WarVKSetFogVolumePosition(v122Fog, x + 50.0, y, z)
        call V122Error(0, "fog-position")
        call WarVKSetFogVolumeDensity(v122Fog, 0.6)
        call V122Error(0, "fog-density")
        call WarVKSetFogVolumeEdgeFeather(v122Fog, 0.3)
        call V122Error(0, "fog-feather")
        call WarVKSetSphereFogVolumeRadius(v122Fog, 750.0)
        call V122Error(0, "sphere-radius")
        call WarVKSetBoxFogVolumeSize(v122Fog, 1.0, 1.0, 1.0)
        call V122Error(19, "shape-mismatch")
        call WarVKDestroyFogVolume(v122Fog)
        call V122Check(not WarVKIsFogVolumeAlive(v122Fog), "fog-stale")
        call WarVKSetFogVolumeDensity(v122Fog, 0.5)
        call V122Error(19, "stale-setter")
        set v122Fog = WarVKCreateBoxFogVolume(x, y, z, 1100.0, 700.0, 550.0, 0.65, 0.2)
        call V122Check(v122Fog > 0 and WarVKIsFogVolumeAlive(v122Fog), "box-create")
        call WarVKSetBoxFogVolumeSize(v122Fog, 1200.0, 750.0, 600.0)
        call V122Error(0, "box-size")
        call WarVKSetFogVolumeRotation(v122Fog, 0.0, 0.0, 35.0)
        call V122Error(0, "box-rotation")
    elseif v122Stage == 5 then
        call WarVKDestroyFogVolume(v122Fog)
        call V122Error(0, "box-destroy")
        set v122Fog = WarVKCreateCylinderFogVolume(x, y, z, 500.0, 600.0, 0.65, 0.2)
        call V122Check(v122Fog > 0 and WarVKIsFogVolumeAlive(v122Fog), "cylinder-create")
        call WarVKSetCylinderFogVolumeSize(v122Fog, 550.0, 700.0)
        call V122Error(0, "cylinder-size")
        call WarVKSetFogVolumeRotation(v122Fog, 15.0, 0.0, 35.0)
        call V122Error(0, "cylinder-rotation")
    elseif v122Stage == 6 then
        call WarVKSetVolumetricBackend(1)
        call V122Error(0, "medium")
    elseif v122Stage == 7 then
        call WarVKSetVolumetricBackend(2)
        call V122Error(0, "high-restored")
        call SetCameraField(CAMERA_FIELD_ANGLE_OF_ATTACK, 346.0, 0.0)
    elseif v122Stage == 8 then
        call WarVKDestroyFogVolume(v122Fog)
        set v122Fog = WarVKCreateSphereFogVolume(x, y, z, 950.0, 0.4, 0.3)
        call V122Check(v122Fog > 0, "inside-sphere")
        call SetCameraField(CAMERA_FIELD_TARGET_DISTANCE, 250.0, 0.0)
    elseif v122Stage == 9 then
        call WarVKDestroyFogVolume(v122Fog)
        call V122Error(0, "last-fog-destroy")
        // Eight slots must remain bounded; ninth creation is rejected.
        loop
            exitwhen i == 8
            set v122Slots[i] = WarVKCreateSphereFogVolume(x, y, z, 50.0, 0.1, 0.2)
            call V122Check(v122Slots[i] > 0, "slot-" + I2S(i))
            set i = i + 1
        endloop
        set excess = WarVKCreateSphereFogVolume(x, y, z, 50.0, 0.1, 0.2)
        call V122Check(excess == 0, "capacity-reject")
        call V122Error(19, "capacity-error")
        set i = 0
        loop
            exitwhen i == 8
            call WarVKDestroyFogVolume(v122Slots[i])
            call V122Error(0, "slot-destroy")
            set i = i + 1
        endloop
        call WarVKSetVolumetricBackend(3)
        call V122Error(19, "invalid-backend")
        call WarVKSetVolumetricEnabled(false)
        call V122Error(0, "final-volume-off")
        call WarVKSetBloomEnabled(true)
        call V122Error(18, "unsupported-bloom-visible-error")
        call PauseTimer(GetExpiredTimer())
        call DestroyTimer(GetExpiredTimer())
    endif
    call V122Receipt()
    set v122Stage = v122Stage + 1
endfunction

function V122Start takes nothing returns nothing
    call DestroyTimer(GetExpiredTimer())
    call TimerStart(CreateTimer(), 7.0, true, function V122Tick)
endfunction
