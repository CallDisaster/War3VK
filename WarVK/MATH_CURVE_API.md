# WarVK 数学程序与公式曲线 API

## 设计边界

`MathProgram` 是一次编译、只读执行的纯数学字节码；`Curve` 保存命名实数参数、坐标
模式和端点锁；闪电模板/实例只保存曲线的不可变快照。公式不能访问文件、网络或游戏
对象，不能创建单位，也没有循环、递归、赋值和动态内存。每个程序固定限制为：

- 公式最多 384 个 ASCII 字节，且不能包含协议分隔符 `;`；
- 最多 16 个作者参数；
- 最多 256 条字节码指令；
- 求值栈最多 64 项，语法嵌套最多 32 层；
- 非有限数、除零和函数定义域错误均令本次求值失败，闪电安全回退到内置中心线。

因此作者不应通过 JASS 循环逐点调用数学 Native。推荐流程始终是：

```text
JASS 编译公式并设置参数
  -> 绑定不可变 Curve 快照
  -> C++ 每帧批量求值
  -> 闪电 Ribbon 消费曲线点
```

## 直接取得科学计算结果

返回 `scalar` 的程序不必绑定闪电。用 `WarVKCreateCurve` 建立参数化计算、通过
`WarVKSetCurveReal` 写入公式参数后，可以调用：

- `WarVKEvaluateMathReal(curveId, t, time, seed)`：返回 JASS `real`；
- `WarVKEvaluateMathInteger(curveId, t, time, seed, roundingMode)`：返回 JASS `integer`。

这两个查询在 `1.2.0 Release` 中优先通过原生 Hashtable 的 `SaveReal`/
`LoadReal` 与 `SaveInteger`/`LoadInteger` 传递数值，不再为每次求值创建命令字符串或
实数返回字符串；公开函数签名保持不变，快速通道不可用时自动回退到 `warvk:v1`。

整数舍入模式为 `WARVK_MATH_ROUND_NEAREST/FLOOR/CEIL/TRUNCATE`。`t` 限制在
0..1，`time` 与 `seed` 作为本次求值上下文；不使用它们的普通科学公式可直接传 0。
公式计算失败、结果非有限或整数越界时接口返回 0，同时设置 WarVK 最后错误，因此
需要区分“正确结果为 0”和失败时应读取 `WarVKGetLastErrorCode()`。

## 表达式语法

第一阶段支持有限浮点数、圆括号、一元正负号、`+ - * /`、函数调用，以及
`float`、`vec2`、`vec3` 三种值类型。向量加减要求类型一致；标量可与向量相乘，
向量可除以标量。公式中不属于内置量的合法标识符会自动成为命名 `real` 参数。

内置常量：`pi`、`tau`。

内置标量上下文：

- `t`：曲线参数，范围 0..1；
- `time`：闪电实例创建后的秒数；
- `length`：起终点直线距离；
- `index`、`segments`：当前采样点和段数；
- `seed`：实例确定性随机种子；
- `branchIndex`、`branchDepth`：主电为 0/0，一级分支为非零/1。

内置 `vec3` 上下文：`start`、`end`、`center`、`direction`、`forward`、
`right`、`up`。

内置函数：

```text
vec2 vec3 x y z
sin cos tan asin acos atan atan2
sqrt pow exp log abs sign floor ceil round fract
min max clamp saturate lerp inverseLerp remap
step smoothstep smootherstep
dot cross length distance normalize project reject rotateAroundAxis
endpointMask noise1 repeat pingpong bezier2 bezier3
```

`noise1(x)` 使用当前 `seed`；`noise1(x, customSeed)` 使用显式种子。它是输入确定的
哈希噪声，不依赖调用顺序，适合自适应采样和联机重建。

## 坐标模式

### OFFSET（默认，推荐闪电）

公式必须返回 `vec2(rightOffset, upOffset)`。底层计算：

```text
P(t) = lerp(start,end,t) + right*x + up*y
```

端点锁会把偏移乘以端点遮罩，保证主电与分支实际连接。

### LOCAL

公式必须返回 `vec3(forwardDistance, rightOffset, upOffset)`。底层以起点为原点、
自动建立 `forward/right/up` 正交基并转换为世界坐标。常用首分量为 `t*length`。

### WORLD

公式必须直接返回世界坐标 `vec3(x,y,z)`。适合固定魔法阵、函数图像和场景轨迹。

LOCAL/WORLD 开启端点锁时，底层会在作者结果与起终点基线之间平滑混合；两端分别
精确落在 `start`、`end`。

## JASS 生命周期

```jass
set programId = WarVKCompileMathProgram("vec2(sin(t*tau*waves)*amp,noise1(t*freq,seed)*amp)")
if programId == 0 then
    call BJDebugMsg(WarVKGetMathProgramLastError())
    return
endif

set curveId = WarVKCreateCurve(programId)
call WarVKSetCurveReal(curveId, "waves", 4.00)
call WarVKSetCurveReal(curveId, "amp", 90.00)
call WarVKSetCurveReal(curveId, "freq", 12.00)
call WarVKSetCurveCoordinateMode(curveId, WARVK_CURVE_COORDINATE_OFFSET)
call WarVKSetCurveEndpointLocks(curveId, true, true)
call WarVKSetLightningTemplateFormulaCurve(templateId, curveId)
```

也可通过 `WarVKSetLightningFormulaCurve(lightningId, curveId)` 给已创建实例绑定快照。
绑定之后，修改或销毁源曲线/程序不会影响模板和实例。若要改变已绑定公式参数，应
修改曲线参数后再次绑定；这一规则避免渲染线程读取 JASS 正在修改的状态。

`WarVKEvaluateCurveComponent`、`WarVKEvaluateCurveDerivativeComponent` 与
`WarVKGetCurveArcLength` 是低频游戏逻辑/调试查询。导数使用有界有限差分；弧长使用
2..256 段离散积分。渲染路径不会调用这些 JASS 查询函数。

## 点曲线与连续 Polyline 闪电

当轨迹已经由 JASS 算出（例如 Lorenz 吸引子、外部样条或寻路结果），无需把每一小段
创建成独立闪电。点曲线采用“分块上传、一次冻结、不可变快照”流程：

```jass
set curveId = WarVKCreatePointCurve(5)
call WarVKAppendPointCurve4(curveId, 4, x0,y0,z0, x1,y1,z1, x2,y2,z2, x3,y3,z3)
// 最后一批只有一个有效点；其余位置重复最后一点并由 pointCount=1 忽略。
call WarVKAppendPointCurve4(curveId, 1, x4,y4,z4, x4,y4,z4, x4,y4,z4, x4,y4,z4)
call WarVKFinalizePointCurve(curveId)
set lightningId = WarVKCreatePolylineLightning(templateId, curveId, 9173)
call WarVKDestroyCurve(curveId)
```

- 点数必须为 2..1024，坐标均为有限世界坐标；
- v1 wire 最多 16 个参数，因此每个 `AppendPointCurve4` 传 1..4 点；
- `Finalize` 只在实际点数恰好等于声明点数且曲线总长度非零时成功；
- 冻结时 C++ 一次性建立累计弧长表，宽度、颜色、脉冲和 UV 都按真实弧长插值；
- 一个 Polyline 对应一个托管闪电实例和一条连续 Triangle Strip，主 Ribbon 一次 draw；
  模板若启用辉光则增加一次 draw，但不会为每段创建对象；
- Polyline 使用作者提供的完整中心线，所以模板分支参数不再额外生成分支；
- `WarVKSetPolylineLightningCurve` 可用另一条已冻结曲线原子替换实例轨迹。

点曲线按世界坐标解释，不使用 OFFSET/LOCAL/WORLD 或端点锁。`EvaluateCurveComponent`
会按归一化弧长查询点曲线，`GetCurveArcLength` 直接返回冻结时烘焙的总长度。

## 渲染顺序与阴影

WarVK 自定义闪电在 Warcraft III 原生 `WorldDispatch S20` 完成后绘制，与原版闪电处于
同一阶段顺序；不会再借用 S11 阴影执行点。S20 被独立标记为 `Lightning/Effect`，并在
所有自定义阴影生产者共用的中央策略中硬拒绝。因此原生 S20 闪电和 WarVK Ribbon 都只
参与主颜色渲染，不会进入 CSM、点阴影或轮廓所消费的阴影几何发布路径。

## 当前与后续阶段

当前已经具备：标量/`vec2`/`vec3`、确定性一维噪声、参数曲线、三种坐标模式、
端点锁、固定有界采样、数值一阶导数、离散弧长、2..1024 点的不可变点曲线，以及
单实例连续 Ribbon 闪电后端。

以下内容暂未宣称实现：`mat2/mat3/mat4`、四元数、比较/三元表达式与局部 `let`、
自动微分、二阶曲率、自适应细分和弧长反查、二维/三维/旋度噪声、RK4/向量场曲线、
节点图与 GPU 批量求值。它们应继续复用同一 `MathProgram -> Curve -> Renderer` 分层，
而不是扩展为不受约束的脚本语言。
