function WarVKBeginPointLightSmokeTest takes real x, real y, real z returns integer
    local integer before = WarVKGetManagedObjectCount()
    local integer lightId = WarVKCreatePointLight(x, y, z, 900.00, 0.25, 0.70, 1.00, 4.00)
    call BJDebugMsg("WarVK version=" + WarVKGetVersion())
    call BJDebugMsg("WarVK protocol=" + I2S(WarVKGetProtocolVersion()) + " flags=" + I2S(WarVKGetFeatureFlags()))
    if lightId > 0 then
        call WarVKSetPointLightPosition(lightId, x, y, z)
        call WarVKSetPointLightColorIntensity(lightId, 0.25, 0.70, 1.00, 4.00)
        call WarVKSetPointLightRadius(lightId, 900.00)
        call WarVKSetPointLightShadowEnabled(lightId, true)
        call WarVKSetPointLightShadowConfig(lightId, 1024, 0.05)
        call BJDebugMsg("WarVK point light id=" + I2S(lightId) + " count=" + I2S(WarVKGetManagedObjectCount()) + " before=" + I2S(before))
        call BJDebugMsg("Keep this light for a screenshot, then call WarVKFinishPointLightSmokeTest.")
    else
        call BJDebugMsg("WarVK point light create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
    endif
    return lightId
endfunction

function WarVKFinishPointLightSmokeTest takes integer lightId returns nothing
    local integer before = WarVKGetManagedObjectCount()
    if lightId > 0 and WarVKIsPointLightAlive(lightId) then
        call WarVKDestroyPointLight(lightId)
        call BJDebugMsg("WarVK point light removed; count=" + I2S(WarVKGetManagedObjectCount()) + " before=" + I2S(before))
    else
        call BJDebugMsg("WarVK smoke cleanup skipped: point light id is not alive.")
    endif
    set lightId = 0
endfunction

function WarVKBeginLightningTemplateSmokeTest takes real startX, real startY, real startZ, real endX, real endY, real endZ returns integer
    local integer templateId = 0
    local integer lightningId = 0
    if not WarVKIsRuntimeReady() then
        call BJDebugMsg("WarVK lightning template smoke skipped: runtime is not ready.")
        return 0
    endif
    // 故意使用中文名，覆盖 GUI 作者最常见的命名方式；API 会把它变成
    // ASCII 的内部传输名，返回 id 才是后续所有配置的依据。
    set templateId = WarVKCreateLightningTemplate("烟雾蓝叉")
    if templateId <= 0 then
        call BJDebugMsg("WarVK template create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
        return 0
    endif
    call WarVKSetLightningTemplateBasic(templateId, "war3mapImported/Lightning.blp", 0.20, 0.70, 1.00, 0.95, 0.90, 0.98, 1.00, 0.75, 30.00, 12.00, 3.00, 0.85, WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH)
    call WarVKSetLightningTemplateAdvanced(templateId, 80.00, 4, 32, 70.00, 36.00, 9.00, 1.00, 2, 2, 1.00, 0.45)
    call WarVKSetLightningTemplateOptional(templateId, 0.00, 0.08, 0.16, 0.25, 7.00, 0.00, 0.08, 11.00, 1.80, 0.22)
    call WarVKFinalizeLightningTemplate(templateId)
    if WarVKGetLastErrorCode() != WARVK_ERROR_NONE then
        call BJDebugMsg("WarVK template finalize failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
        return 0
    endif
    set lightningId = WarVKCreateLightningFromTemplate(templateId, startX, startY, startZ, endX, endY, endZ, 9173)
    if lightningId <= 0 then
        call BJDebugMsg("WarVK template instance create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
    else
        call BJDebugMsg("WarVK lightning template=" + I2S(templateId) + " instance=" + I2S(lightningId) + "; keep it for a screenshot, then call WarVKFinishLightningTemplateSmokeTest.")
    endif
    return lightningId
endfunction

function WarVKFinishLightningTemplateSmokeTest takes integer lightningId returns nothing
    if lightningId > 0 and WarVKIsLightningAlive(lightningId) then
        call WarVKDestroyLightning(lightningId)
        call BJDebugMsg("WarVK lightning template smoke instance removed.")
    endif
    set lightningId = 0
endfunction

// 公式曲线验收：创建一条两端锁定、随时间旋转的局部螺旋闪电。
// 返回实例 id；截图后仍使用 WarVKFinishLightningTemplateSmokeTest 清理。
function WarVKBeginFormulaLightningSmokeTest takes real startX, real startY, real startZ, real endX, real endY, real endZ returns integer
    local integer programId = 0
    local integer curveId = 0
    local integer templateId = 0
    local integer lightningId = 0
    if not WarVKIsRuntimeReady() then
        call BJDebugMsg("WarVK formula lightning smoke skipped: runtime is not ready.")
        return 0
    endif
    set programId = WarVKCompileMathProgram("vec2(cos(t*turns*tau+time*speed)*radius,sin(t*turns*tau+time*speed)*radius)")
    if programId <= 0 then
        call BJDebugMsg("WarVK formula compile failed: " + WarVKGetMathProgramLastError())
        return 0
    endif
    set curveId = WarVKCreateCurve(programId)
    call WarVKSetCurveReal(curveId, "turns", 3.00)
    call WarVKSetCurveReal(curveId, "speed", 1.50)
    call WarVKSetCurveReal(curveId, "radius", 100.00)
    call WarVKSetCurveCoordinateMode(curveId, WARVK_CURVE_COORDINATE_OFFSET)
    call WarVKSetCurveEndpointLocks(curveId, true, true)

    set templateId = WarVKCreateLightningTemplate("公式螺旋")
    call WarVKSetLightningTemplateBasic(templateId, "ReplaceableTextures\\Weather\\Lightning.blp", 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 34.00, 18.00, 4.00, 0.80, WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH)
    call WarVKSetLightningTemplateAdvanced(templateId, 40.00, 16, 64, 0.00, 0.00, 0.00, 0.00, 1, 0, 1.00, 0.45)
    call WarVKSetLightningTemplateOptional(templateId, 0.00, 0.08, 0.16, 0.15, 5.00, 0.00, 0.00, 0.00, 1.60, 0.18)
    call WarVKSetLightningTemplateFormulaCurve(templateId, curveId)
    call WarVKFinalizeLightningTemplate(templateId)
    set lightningId = WarVKCreateLightningFromTemplate(templateId, startX, startY, startZ, endX, endY, endZ, 9173)

    // 模板已经保存不可变快照，临时作者句柄可以立即释放。
    call WarVKDestroyCurve(curveId)
    call WarVKDestroyMathProgram(programId)
    if lightningId <= 0 then
        call BJDebugMsg("WarVK formula lightning create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
    else
        call BJDebugMsg("WarVK formula lightning instance=" + I2S(lightningId) + "; keep it for a screenshot, then clean it up.")
    endif
    return lightningId
endfunction

// 经典 Lorenz 吸引子：sigma=10、rho=28、beta=8/3，Euler dt=0.01。
// JASS 只在创建时采样并分块上传；冻结后 C++ 用一个连续 Ribbon 渲染全部640点。
// 返回的仍是一个闪电实例 id，可用 WarVKFinishLightningTemplateSmokeTest 清理。
function WarVKBeginLorenzPolylineSmokeTest takes real centerX, real centerY, real baseZ returns integer
    local integer pointCount = 640
    local integer burnIn = 1000
    local integer i = 0
    local integer curveId = 0
    local integer templateId = 0
    local integer lightningId = 0
    local real sigma = 10.00
    local real rho = 28.00
    local real beta = 2.6666667
    local real dt = 0.01
    local real x = 0.10
    local real y = 0.00
    local real z = 0.00
    local real nx = 0.00
    local real ny = 0.00
    local real nz = 0.00
    local real x0 = 0.00
    local real y0 = 0.00
    local real z0 = 0.00
    local real x1 = 0.00
    local real y1 = 0.00
    local real z1 = 0.00
    local real x2 = 0.00
    local real y2 = 0.00
    local real z2 = 0.00
    local real x3 = 0.00
    local real y3 = 0.00
    local real z3 = 0.00

    if not WarVKIsRuntimeReady() then
        call BJDebugMsg("WarVK Lorenz polyline smoke skipped: runtime is not ready.")
        return 0
    endif

    // 丢弃初始过渡段，让采样直接落在吸引子上。
    loop
        exitwhen i >= burnIn
        set nx = x + sigma * (y - x) * dt
        set ny = y + (x * (rho - z) - y) * dt
        set nz = z + (x * y - beta * z) * dt
        set x = nx
        set y = ny
        set z = nz
        set i = i + 1
    endloop

    set curveId = WarVKCreatePointCurve(pointCount)
    if curveId <= 0 then
        call BJDebugMsg("WarVK Lorenz point curve create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
        return 0
    endif
    set i = 0
    loop
        exitwhen i >= pointCount
        // 一次积分四步并立即上传；无需在 JASS 侧保存640点数组。
        set nx = x + sigma * (y - x) * dt
        set ny = y + (x * (rho - z) - y) * dt
        set nz = z + (x * y - beta * z) * dt
        set x = nx
        set y = ny
        set z = nz
        set x0 = centerX + x * 8.00
        set y0 = centerY + y * 8.00
        set z0 = baseZ + (z - 25.00) * 3.00
        set nx = x + sigma * (y - x) * dt
        set ny = y + (x * (rho - z) - y) * dt
        set nz = z + (x * y - beta * z) * dt
        set x = nx
        set y = ny
        set z = nz
        set x1 = centerX + x * 8.00
        set y1 = centerY + y * 8.00
        set z1 = baseZ + (z - 25.00) * 3.00
        set nx = x + sigma * (y - x) * dt
        set ny = y + (x * (rho - z) - y) * dt
        set nz = z + (x * y - beta * z) * dt
        set x = nx
        set y = ny
        set z = nz
        set x2 = centerX + x * 8.00
        set y2 = centerY + y * 8.00
        set z2 = baseZ + (z - 25.00) * 3.00
        set nx = x + sigma * (y - x) * dt
        set ny = y + (x * (rho - z) - y) * dt
        set nz = z + (x * y - beta * z) * dt
        set x = nx
        set y = ny
        set z = nz
        set x3 = centerX + x * 8.00
        set y3 = centerY + y * 8.00
        set z3 = baseZ + (z - 25.00) * 3.00
        call WarVKAppendPointCurve4(curveId, 4, x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3)
        set i = i + 4
    endloop
    call WarVKFinalizePointCurve(curveId)
    if WarVKGetLastErrorCode() != WARVK_ERROR_NONE then
        call BJDebugMsg("WarVK Lorenz point curve finalize failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
        call WarVKDestroyCurve(curveId)
        return 0
    endif

    set templateId = WarVKCreateLightningTemplate("Lorenz连续蝴蝶")
    call WarVKSetLightningTemplateBasic(templateId, "ReplaceableTextures\\Weather\\Lightning.blp", 0.35, 0.75, 1.00, 0.95, 0.90, 0.98, 1.00, 0.75, 18.00, 10.00, 12.00, 0.25, WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH)
    // 点曲线已经是完整中心线：不再叠加噪声、弧度或模板分支。
    call WarVKSetLightningTemplateAdvanced(templateId, 20.00, 2, 64, 0.00, 0.00, 0.00, 0.00, 1, 0, 1.00, 0.45)
    call WarVKSetLightningTemplateOptional(templateId, 0.00, 0.08, 0.16, 0.10, 3.00, 0.20, 0.00, 0.00, 1.50, 0.16)
    call WarVKFinalizeLightningTemplate(templateId)
    set lightningId = WarVKCreatePolylineLightning(templateId, curveId, 9173)
    call WarVKDestroyCurve(curveId)

    if lightningId <= 0 then
        call BJDebugMsg("WarVK Lorenz polyline create failed: " + I2S(WarVKGetLastErrorCode()) + " " + WarVKGetLastError())
    else
        call BJDebugMsg("WarVK Lorenz polyline instance=" + I2S(lightningId) + " points=" + I2S(pointCount) + "; one managed lightning, continuous Ribbon.")
    endif
    return lightningId
endfunction
