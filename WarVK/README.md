# WarVK JAPI

WarVK 的运行时内置于 DXVK `d3d9.dll`。地图通过 Warcraft III 自带的
`Preloader`、`GetLocalizedHotkey` 和 `GetLocalizedString` 调用
`warvk:v1` 协议，不声明额外 Native，也不需要 `war3map.dll` 提供这套接口。

从 `1.2.0 Release` 起，`warvk:v1` 继续作为兼容和低频控制面；高频纯数值接口
优先走 Warcraft 原生 Hashtable Native 组成的强类型数据面。JASS 包装函数名和参数完全
不变，旧 DLL 或快速通道验证失败时会自动回退到字符串协议。

## JAPI 技术路线

强类型数据面不注册新 Native，也不借用其他 JAPI 项目的逐函数注册表。DXVK 仅对
`SaveInteger`、`SaveReal`、`LoadInteger`、`LoadReal` 建立一个可选的第二安装事务，并在
运行时严格核对 Game.dll Native 表中的函数签名。JASS 创建一张私有 Hashtable，通过两步
能力握手取得本地图会话的数据面权限；每次调用再以序号、操作码、定长参数槽和 commit/
query 组成原子事务。只有精确匹配私有表、当前事务和声明参数类型的调用会被消费，其余
Hashtable 调用全部转发给魔兽原函数。

当前优先迁移了会频繁更新或频繁查询的接口：点光位置/颜色/强度/半径、闪电端点/颜色/
宽度、数学公式和曲线的数值求值、点曲线四点批量上传，以及视觉时间/FPS/帧耗时查询。
它们不再执行 JASS `I2S`/`R2S`、字符串拼接或动态 JASS 实数结果字符串转换。模板名称、
贴图路径、公式源码等真正需要文本的低频接口仍使用 `warvk:v1`，因此不会为了消灭字符串
而引入二进制文本编码。

地图/VM 重建时，C++ 会撤销 Hashtable 能力并丢弃未提交事务；新的地图必须重新握手。
四个可选 Native 中任一地址或签名不匹配时，第二事务整体不安装，但原来的三个字符串
Carrier 保持可用。

## 目录

- `action.txt`、`call.txt`、`define.txt`：YDWE MapUI 定义（含参数 comment 提示）。
- `jass/warvk_init.j`：地图侧入口，会引入桥与全部公开 API。
- `jass/warvk_api.j`：`warvk:v1` 命令的 JASS 包装（闪电模板与公式曲线参数有完整注释）。
- `jass/warvk_constant.j`：公开常量（含 `WARVK_LIGHTNING_RENDER_*`）。
- `MATH_CURVE_API.md`：公式语法、内置变量/函数、坐标模式和阶段边界。
- `jass/warvk_smoke_test.j`：点光与闪电模板的可选验收函数（不自动 include）。
- `loader/warvk.ai`：地图内 AI 加载路线。
- `loader/warvk_loader.lua`：Lua 加载兼容路线。
- `package_warvk.ps1`：生成 `WarVK.dll` 与地图载荷。

## YDWE 接入

把整个根目录作为一个独立的 `ui/WarVK` 层接入 YDWE；至少保留根目录的
`action.txt`、`call.txt`、`define.txt` 和 `jass`。不要把 action 定义复制到
`MapUI` 后再改写返回类型。编辑器中的函数 `comment` 已写明参数含义与有效范围。
地图预处理入口包含 `jass/warvk_init.j`。如果使用地图内加载路线，同时导入
`warvk.ai` 和由构建脚本生成的 `warvk.blp`。

DXVK 已作为代理 DLL 启动时，初始化函数会直接检测到 bridge；无需再次加载 DLL。

YDWE 菜单按功能拆分为 WarVK 系统/诊断、太阳与阴影、光照时钟与昼夜、点光源、体积光、体积雾、
闪电、闪电模板、数学公式和曲线。根 `WarVK` 分类只保留版本、协议、功能位与运行时
状态；没有 C++ dispatch 的预留接口不会出现在作者菜单中。

## 点光源、体积光与全局高度雾

`WarVKSetPointLightPosition(lightId, x, y, z)` 会更新已有托管点光的位置；创建函数返回的
`lightId` 在销毁前保持有效，因此可由计时器或技能逻辑持续移动。同一组接口还可以更新
颜色、强度、半径和点阴影。

体积光与全局高度雾分别控制：

```jass
call WarVKSetVolumetricEnabled(true)
call WarVKSetVolumetricDensity(0.80)
call WarVKSetVolumetricScattering(1.50, 0.95)
call WarVKSetVolumetricQuality(16, 1400.00)

call WarVKSetGlobalVolumetricFogEnabled(true)
call WarVKSetGlobalVolumetricFog(0.00, 0.0012, 0.35)
```

高度雾开关不会自动开启体积光通道；关闭高度雾也不会关闭太阳光和点光在介质中的散射。
当前开放的是覆盖全图的高度雾。按 Box/Sphere/Cylinder 在指定区域创建的局部雾体积尚未
实现，因此本版不提供无效果的局部雾创建函数。

## 光照时钟、天体轨迹与时间色温

`1.2.0 Release` 将原先每帧捆绑更新的太阳方向和色温拆成两个独立控制器。光照时钟
只影响 WarVK 渲染，不会修改 Warcraft 的玩法时间：

```jass
// 完全手动：作者设置的太阳方向和颜色不会再被下一帧覆盖。
call WarVKSetCelestialMotionEnabled(false)
call WarVKSetTimeColorGradingEnabled(false)
call WarVKSetSunDirection(-0.35, -0.80, -0.48)
call WarVKSetSunColorIntensity(1.00, 0.82, 0.68, 1.20)

// 固定太阳方向，但让色温按作者指定的下午时刻计算。
call WarVKSetCelestialMotionEnabled(false)
call WarVKSetTimeColorGradingEnabled(true)
call WarVKSetLightingClockTime(16.50)

// 自定义00:00、06:00、12:00、18:00的循环色温关键点（Kelvin）。
call WarVKSetTimeColorTemperatureProfile(9000.00, 2800.00, 6200.00, 2400.00)

// 独立渲染昼夜：从08:00开始，每240秒走完一整天。
call WarVKSetLightingClockTime(8.00)
call WarVKSetLightingDayDuration(240.00)
call WarVKSetLightingClockMode(WARVK_LIGHTING_CLOCK_INDEPENDENT)
```

`WarVKSetLightingClockTime` 接受0..24并自动进入保持模式。时钟模式可以选择跟随游戏时间、
保持作者时刻或独立推进。色温曲线通过00:00、06:00、12:00、18:00四个1000..20000K
采样点平滑循环插值；`WarVKResetTimeColorTemperatureProfile` 恢复内置自然曲线。

## 闪电模板

模板由地图 JASS / YDWE 触发器创建，不依赖 Warcraft 原生闪电类型。请在地图
初始化或技能触发里直接调用 API（不必再维护单独的“模板分区”库文件）：

```jass
// 1. 创建（名称可用中文；实际请保存返回的 templateId）
set udg_MyBoltTemplate = WarVKCreateLightningTemplate("蓝色闪电")

// 2. 基础：贴图 / 首尾颜色(0..1) / 宽度 / UV / 渲染模式
call WarVKSetLightningTemplateBasic(udg_MyBoltTemplate, "ReplaceableTextures\\Weather\\Lightning.blp", 0.20, 0.70, 1.00, 0.95, 0.90, 0.98, 1.00, 0.75, 30.00, 12.00, 3.00, 0.85, WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH)

// 3. 进阶：分段、弧度、噪声、分叉
call WarVKSetLightningTemplateAdvanced(udg_MyBoltTemplate, 80.00, 4, 32, 70.00, 36.00, 9.00, 1.00, 2, 2, 1.00, 0.45)

// 4. 可选：寿命、淡入淡出、脉冲、闪烁、辉光（lifetime=0 表示常驻）
call WarVKSetLightningTemplateOptional(udg_MyBoltTemplate, 0.00, 0.08, 0.16, 0.25, 7.00, 0.00, 0.08, 11.00, 1.80, 0.22)

// 5. 冻结（之后不可再改模板）
call WarVKFinalizeLightningTemplate(udg_MyBoltTemplate)

// 6. 按模板创建实例
set udg_Bolt = WarVKCreateLightningFromTemplate(udg_MyBoltTemplate, x1, y1, z1, x2, y2, z2, 9173)
```

| 分区 | 参数概要 |
| --- | --- |
| 基础 | 贴图、首尾 RGBA（Alpha 0..1）、首尾宽度 1..4096、UV 平铺/流速、renderMode 0..3 |
| 进阶 | 段长 4..8192、段数 2..64、弧度/噪声、octaves 1..4、分支 0..8 |
| 可选 | 寿命 0..600（0=常驻）、淡入淡出、脉冲、闪烁、辉光宽度倍率 1..8 |

贴图路径必须是相对地图/MPQ 路径。当前直接解码 BLP1 JPEG / BLP1 索引色，并支持
TGA、PNG、JPG/JPEG、BMP；**BLP2 尚未支持**。标准 Warcraft BLP1 JPEG 没有 Alpha
平面，闪电渲染器会依据贴图边缘背景自动生成软透明遮罩；需要精确透明形状时请使用
带 Alpha 的 PNG 或 TGA。

模板配置只写入 DXVK 侧 CPU 描述，可在 bridge 可用后立即完成，不必等首个渲染帧；
贴图在首次绘制时再加载。实例会复制冻结时的参数，之后改模板不影响已创建闪电。

参数细节见 `jass/warvk_api.j` 函数上方注释，以及 YDWE 动作/函数的 `comment` 字段。

## 公式曲线

第一阶段已经把数学能力从闪电渲染器中独立出来：JASS 只提交一次公式和参数，C++
将公式编译为有界字节码并在渲染时批量生成整条中心线，不会每个点回调 JASS。

```jass
// OFFSET 模式：公式返回 vec2(右偏移, 上偏移)。底层自动补上起终点直线。
set udg_HelixProgram = WarVKCompileMathProgram("vec2(cos(t*turns*tau+time*speed)*radius,sin(t*turns*tau+time*speed)*radius)")
set udg_HelixCurve = WarVKCreateCurve(udg_HelixProgram)
call WarVKSetCurveReal(udg_HelixCurve, "turns", 5.00)
call WarVKSetCurveReal(udg_HelixCurve, "radius", 120.00)
call WarVKSetCurveReal(udg_HelixCurve, "speed", 2.00)
call WarVKSetCurveCoordinateMode(udg_HelixCurve, WARVK_CURVE_COORDINATE_OFFSET)
call WarVKSetCurveEndpointLocks(udg_HelixCurve, true, true)

// 必须在模板冻结前绑定。绑定的是当前参数的不可变快照。
call WarVKSetLightningTemplateFormulaCurve(udg_MyBoltTemplate, udg_HelixCurve)
call WarVKFinalizeLightningTemplate(udg_MyBoltTemplate)

// 临时句柄可回收；模板及其后续实例仍持有安全快照。
call WarVKDestroyCurve(udg_HelixCurve)
call WarVKDestroyMathProgram(udg_HelixProgram)
```

三种模式分别为 OFFSET（`vec2` 偏移，最适合闪电）、LOCAL（沿光束局部基返回
`vec3`）与 WORLD（直接返回世界 `vec3`）。`time` 是实例创建后的秒数；`seed`、
`branchIndex`、`branchDepth` 可用于确定性分支变化。详细语法和完整函数表见
`MATH_CURVE_API.md`。

数学程序也可完全脱离闪电使用。标量公式创建参数化计算后，作者可以直接取得 JASS
`real` 或 `integer`：

```jass
set udg_Program = WarVKCompileMathProgram("mass*speed*speed*0.5")
set udg_Calculation = WarVKCreateCurve(udg_Program)
call WarVKSetCurveReal(udg_Calculation, "mass", 12.00)
call WarVKSetCurveReal(udg_Calculation, "speed", 25.00)
set udg_ResultReal = WarVKEvaluateMathReal(udg_Calculation, 0.00, 0.00, 0)
set udg_ResultInt = WarVKEvaluateMathInteger(udg_Calculation, 0.00, 0.00, 0, WARVK_MATH_ROUND_NEAREST)
```

这两个查询只接受返回 `scalar` 的公式。整数查询支持最近整数、向下、向上和截断四种
明确舍入方式；失败或超出 int32 范围时返回 0，并可立即读取最后错误码区分合法的 0。

## 点曲线与 Lorenz 等采样轨迹

已经算好的世界坐标点可使用 `WarVKCreatePointCurve`、`WarVKAppendPointCurve4`、
`WarVKFinalizePointCurve`，再交给 `WarVKCreatePolylineLightning`。最多 1024 点；上传因
v1 协议限制按每批四点进行，但冻结后整条轨迹是一个不可变快照、一个托管闪电实例和
一条连续 Ribbon，不会产生数百个分段闪电对象。现有实例可通过
`WarVKSetPolylineLightningCurve` 原子替换轨迹。

自定义闪电现在跟随 Warcraft III 原生闪电的 S20 世界阶段，在原生 S20 完成后绘制。
S20 被独立分类为 Lightning/Effect，并由中央阴影生产者策略硬拒绝：原版闪电和 WarVK
连续 Ribbon 均不会发布到 CSM 或点阴影 caster 路径。

## 验收

`jass/warvk_smoke_test.j` 提供可选验收函数（需地图自行 include，不随 init 自动加载）：

```jass
local integer lightId = WarVKBeginPointLightSmokeTest(x, y, z)
// 保留至少一个渲染帧并截图
call WarVKFinishPointLightSmokeTest(lightId)
```

闪电模板可用 `WarVKBeginLightningTemplateSmokeTest` 创建一条保留到下一次
手动清理的实例，再以 `WarVKFinishLightningTemplateSmokeTest` 回收。
公式曲线可用 `WarVKBeginFormulaLightningSmokeTest` 创建一条旋转螺旋闪电，并用
同一个 Finish 函数回收。
`WarVKBeginLorenzPolylineSmokeTest` 会在 JASS 中采样经典 Lorenz 吸引子，再作为一个
连续闪电实例提交；它只用于地图作者手动验收，不由 WarVK 初始化自动运行。

验收信息包括 API 版本、协议版本、功能位、正数对象 ID、对象计数变化以及点阴影。
当前实现的功能位为 `0x7F0F`：Sun、CSM、PointLight、Volumetric、DayNight、Lightning、
ManagedObject、Time、Stats、MathCurve 和 PolylineCurve。未接通的功能会返回
`WARVK_ERROR_UNSUPPORTED_FEATURE`。
