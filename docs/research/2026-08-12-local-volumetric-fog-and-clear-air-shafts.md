# 局部体积雾与清澈空气体积阴影（2026-08-12）

## 目标与边界

本阶段在既有 quarter-resolution 体积光 pass 中增加有界的空间介质描述层：地图作者可创建
Sphere、Box、Cylinder 三种局部体积雾；方向光继续使用已发布 CSM，点光继续使用已发布 cube
shadow。目标观感是低全局雾量下仍能看清遮挡物沿光传播方向形成的暗体积柱，而不是把全场抬成
均匀灰雾。

本阶段不构建三维 extinction texture，也不让雾体积互相投影。局部雾会接收现有实体阴影，但不会
遮挡另一个雾体积；后者属于 froxel extinction / lighting volume 的后续范围。最多同时保留 8 个
有效体积，越界创建失败。

## 一手依据

1. Bartlomiej Wronski, *Volumetric Fog: Unified, Compute Shader Based Solution to Atmospheric
   Scattering*, SIGGRAPH 2014：将介质密度、逐体素光照/阴影与视线积分分成独立阶段，并说明局部
   体积和 clustered culling 是空间介质的自然表达。
   <https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf>
2. Sébastien Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH
   2015 / EA：统一 extinction volume、局部介质元素、透射率与体积阴影。
   <https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite?isLocalized=true>
3. NVIDIA, *Fast, Flexible, Physically-Based Volumetric Light Scattering*：以 Beer-Lambert
   透射率、消光与光源 visibility 描述体积散射积分，并明确阴影改变的是入射光可见性。
   <https://developer.nvidia.com/sites/default/files/akamai/gameworks/downloads/papers/NVVL/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf>
4. GPU Gems 3, Chapter 13, *Volumetric Light Scattering as a Post-Process*：屏幕空间体积遮挡
   可以强化光柱，但遮挡项必须和介质能量分开理解。
   <https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process>
5. Khronos, *Vulkan Specification*：descriptor set / uniform buffer 的布局和访问必须与 shader
   接口一致；本实现使用既有 scalar-layout 合同，并以 C++ `static_assert` 锁定 528-byte UBO。
   <https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html>

上述资料授权的是物理量分离、空间介质、透射率和 visibility 的结构，不授权具体艺术默认值。
本轮 clear-air 默认值是针对 Warcraft III 画面的候选，需要玩家前台 A/B 才能成为 Release 视觉结论。

## 介质公式

沿相机射线 `P(s) = C + sD`，总消光系数写为：

```text
sigma_t(P) = I_global * sigma_global(P) + sum_i sigma_i * W_i(P)
sigma_i     = density_response(density_i) * 0.0006
T(a,b)      = exp(- integral[a,b] sigma_t(P(s)) ds)
delta_L     = T(0,a) * (1 - T(a,b)) * albedo * phase * light * visibility
```

`density_response(d) = clamp(d,0,2) / (1 + 0.5*clamp(d,0,2))` 延续现有饱和映射，避免外部最大
值制造不透明雾墙。所有介质共享既有由 `decay` 映射的 single-scattering albedo；局部体积只增加
空间消光，不创建另一套不可控的能量模型。重叠体积按消光相加，因此仍满足 Beer-Lambert 组合。

`I_global` 是独立的 0/1 作者开关。它只令全局项归零，不改写作者设置的 density，也不清除局部
体积；因此可以在局部雾调试期间来回切换，而不需要用 `density=0` 破坏原全局参数。

全局介质保留原高度函数：

```text
sigma_global(P) = sigma_global_base * height_profile(P.z) * clear_air_presence
```

局部介质不应用全局 near fade。全局 density=0 时，体积 pass 仍可由局部介质独立驱动；体积外
`sigma_t=0`，所以不会凭空生成全场雾幕。

## 精确支撑区间

固定 16 步只在步中点判断形状会漏掉比一步更薄的体积。本实现先把世界射线变换到标准局部形状，
再求解析进入/离开区间；每个 march segment 积分它与体积区间的实际重叠长度。

CPU 使用 `R = Rz * Ry * Rx`。三条 world-to-local 行分别是旋转轴除以半尺寸，并把平移写入
`w = -dot(row.xyz, center)`。标准形状为：

- Sphere：`dot(p,p) <= 1`，解二次方程；
- Box：`abs(p.xyz) <= 1`，用三轴 slab clipping；
- Cylinder：`dot(p.xy,p.xy) <= 1 && abs(p.z) <= 1`，用二维二次方程加 Z slab。

边缘过渡 `f` 在标准局部空间计算。`f=0` 为硬边界；否则对距边界的归一化距离做 smoothstep。
支撑区间始终使用解析边界，feather 只调节区间内部密度，不会让细体积因为采样中点落在外部而消失。

## 阴影柱与“周围几乎无雾”

现有 shader 分别累计未受实体遮挡的太阳参考能量 `L_ref` 与使用 CSM visibility 的物理能量
`L_lit`。整条射线的真实体积遮挡为：

```text
O_path = 1 - clamp(L_lit / max(L_ref, epsilon), 0, 1)
```

`weight` 只重映射 `O_path` 的可读性，不提高全局介质能量；这样角色到地面投影之间可以形成暗柱，
而柱外空气仍由低 density / extinction 控制。候选默认改为低全局 density、低 extinction、关闭全局
height fog，并降低无阴影 fallback；这组数值必须通过同一镜位前台 A/B 检查曝光、天空与地形。

### 视角无关的局部遮挡证据

旧候选会按 `abs(dot(viewRay, worldUp))` 改变探针间距、peak 混合、对比指数和底图加深上限。这不是
介质或可见性公式的一部分，会使同一个阴影柱在俯视时清楚、压低镜头后消失。新候选对每个积分段
保留同源的参考/物理 visibility 差：

```text
DeltaV_j = max(V_ref,j - V_physical,j, 0)
E_shadow = sum_j tau_j * DeltaV_j
O_local  = max_j clamp(DeltaV_j / max(V_ref,j, epsilon), 0, 1)
G        = smoothstep(0.00035, 0.0060, E_shadow)
O        = max(O_path, 0.68 * G * O_local)
```

`E_shadow` 要求遮挡同时拥有真实 CSM 差值与非零介质光学支撑，防止单个噪声探针留下脱离雾体的
黑贴花。最终可读性指数与底图加深上限不再依赖相机俯仰；volume-sun 只保留有界的 0.74 系数差。
纵向 CSM 探针间距统一为 10 世界单位，每个 march segment 仍最多 8 个探针，级联接缝最多 16 次
原始读取，未放宽循环硬上限。这是对 Wronski / NVIDIA 结构中“介质支撑 × 光源 visibility”的直接
映射；0.68、阈值与 24% 底图上限是 Warcraft III 的艺术候选，不是论文常数。

### 远距离局部体积的空间采样

quarter-resolution 全屏 pass 中，一个远处局部体积可能小于一个 effect pixel。清澈空气模式下，
非零结果严格局限于局部体积保守包围球的屏幕 union，因此新候选允许内部使用 ceil-half 目标加
scissor ROI。只有在作者请求的完整 sample count 同时满足下面两条时才启用：

```text
ROI_pixels * samples <= 4,000,000
ROI_pixels * samples * fogCount * (1 + pointCount) <= 96,000,000
```

否则仍回退到既有 divisor=4..8 全屏路径；不会以减少纵向步数为代价强行提升空间分辨率。Vulkan
render area 仍清除完整 attachment，实际 fragment 与 composite 由保守扩张 scissor 限定，预算按
实际提交的 ROI pixel 计算。该路径只在全局介质关闭且局部体积存在时生效，不能改变全局雾的成本。

## WarVK 映射与安全合同

- JASS/JAPI/API 只修改 CPU manager；渲染线程在帧边界复制固定上限不可变 snapshot，不回调 JASS。
- 所有坐标、旋转、密度、feather、尺寸在 JAPI 和 manager 两层检查有限性与范围。
- UBO 使用 24 槽 ring，与现有 CSM/point UBO 一致；descriptor binding 6 只在当前 draw 绑定。
- 地图会话 reset 在 Present 安全点清空 CPU fog manager。GPU UBO 不被 Hook/JASS 线程直接释放，
  而是继续由设备/渲染 pass 生命周期拥有。
- CPU 先以保守 bounding sphere 做 viewport culling；shader 最多测试 8 个体积。
- 全局介质与高度分布是两个独立开关；关闭全局介质仍允许局部体积驱动 pass。
- ray-segment watchdog 仍为 4,000,000；另设 96,000,000 次局部介质交互预算，计入体积数和所选
  点光数。最低 4 步仍超预算时跳过可选 pass，而不是无界执行。
- 缺失/无效 CSM 只使太阳体积 fail-soft；不会伪造实体阴影。点光仍按其独立 cube snapshot 合同运行。

## 验收要求

离线门应覆盖解析变换、容量/输入边界、JAPI 句柄隔离、reset、UBO ABI、shader 解析求交和
global-density-zero 路径。DLL 构建只能证明接口和 shader 编译闭合；视觉结论仍需至少：

1. 固定镜位比较旧默认与 clear-air 默认；
2. global density=0，分别验证 Sphere/旋转 Box/旋转 Cylinder；
3. 细体积小于单步长度时不闪失；
4. 角色/建筑 CSM 在局部雾内形成连续暗柱，体积外无雾；
5. 点光 cube shadow 在局部雾内生效；
6. 新进程单地图长巡航无 budget exceeded / device lost / dump。

跨地图与点阴影摩尔纹仍受 1.2.0 已知问题边界约束，不能由本次局部雾离线测试外推为已修复。
