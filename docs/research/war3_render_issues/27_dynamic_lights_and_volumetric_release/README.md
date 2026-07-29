# 动态点光源、点阴影与体积光

最后更新：2026-07-16

## 2026-07-16 长期线：Volume Sun Shadow（与相机 CSM 解耦）

**问题**：体积太阳柱此前完全消费相机相对 CSM。俯仰变化时，空气柱中段是否落在 cascade
覆盖内会变，表现为柱从模型「长」到地、抬高缩回模型；表面近清远糊也同步传染到体积。

**合同（P1 已落地代码）**：

| 角色 | 阴影源 |
|---|---|
| 表面 receiver / 地面阴影 | 相机相对 CSM（行为不变） |
| 体积太阳柱遮挡 | **Volume Sun Shadow**：单层太阳对齐 ortho，固定世界半径 |

- 中心：相机世界位置；半径默认 3000（64 步进量化），不随 pitch/FOV 膨胀。
- 深度：margin + 向太阳侧 extension（默认 512）；resolution 默认 1024。
- 生产：`War3ShadowReceiverPass::renderVolumeSunShadow`，复用本帧 replay casters。
- 消费：`GetVolumetricSunShadowSnapshot`；`war3_volumetric_light.frag` 在
  `u_volumeSunParams.x>0.5` 时以 cascadeCount=1 + 3x3 soft 采样为主路径。
- 回退：`volumeSunShadowFallbackToCsm`（默认 true）仅在 volume sun 无效时用旧 CSM。
- 体积 `enabled=false` 时不分配、不绘制 volume sun（零成本）。

**设置字段**（`War3VolumetricLightSettings`）：
`volumeSunShadowEnabled`、`volumeSunShadowFallbackToCsm`、`volumeSunOrthoRadius`、
`volumeSunDepthExtension`、`volumeSunDepthMargin`、`volumeSunResolution`、
`volumeSunSoftRadius`、`volumeSunReceiverBias`。

**验收（未跑实机前不得宣称通过）**：固定太阳下上帝俯视与半压镜头，柱应连续贴地，
不随 pitch 从模型长缩。

**后续（P3/P4，未做）**：2 级固定半径 volume cascade；轻量 froxel 统一雾。

## 当前结论

三条路径仍默认关闭，但已经从早期实验代码推进到可配置、可诊断并具备发布安全门的实现：

- 点光直接照明：使用不可变同帧快照，先过滤非法/零能量灯，再按“投影阴影优先 +
  `maxRGB * intensity * range² / (1 + cameraDist²)`”形成 canonical 顺序。
- cube 点阴影：默认单灯 1024，可选 2048；只有六个 face 全部有效时才允许采样，未完整时保留直接光。
- 体积光：默认 quarter-resolution（`resolutionDivisor=4`、16 steps），使用
  Beer-Lambert 单次散射、CSM 边界 feather、consumer-local relevance top-2 点光与
  LDR soft-headroom 合成。
- 软件点光 contact ray：A0 线性步进与 A1 半分辨率 Hi-Z 均已实现，独立记录在
  [29 号文档](../29_hybrid_ray_tracing/README.md)，用于补 cube 阴影遗漏的近接触细节。
  A1 默认仍为 1 灯、上限 2 灯；两灯重叠像素访问总和硬限
  `configured+8`，默认 24 visits 时最多 32。

2026-07-14 本批代码与独立静态复审已收口，P0/P1=0。最新合并 build-only 为
`gpu_skin_light_build_only_20260714_123117`；56/56 PASS，修改过的
`war3_shadow_receiver.frag`、`war3_volumetric_light.frag` 均重新生成，GPU skin manager、
device、Volumetric C++ 与最终 x86 link 均实际执行。error=0；92 行 warning 中 90 行为
既有 `OPCode -Wreorder`，2 行为无关 `war3_render_exec_batch` unused variable，本轮目标
单元无直接 warning。DLL SHA256 为
`0620F24CDB7BDCB3D44ADBDDA8431C59571257CC50A475485E05C259D1D71E83`，且
`launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`。
但 **尚未执行本轮运行画质或性能验收**。Warcraft III 当前由地图开发占用，不能把旧截图、旧 FPS 或静态检查当成新算法已经视觉通过。

## 点阴影当前实现

关键契约：

1. `War3LightManager::GetFrameSnapshot` 在同帧返回不可变值快照；投射阴影的灯位于 `[0, shadowCount)`，再按重要度排序。
2. 点光直接 UBO 与点阴影 CPU 计划消费同一 generation/order，避免“光在 A、阴影在 B”。
3. caster 与 receiver 均以 `|P-L| / range` 写入和比较线性径向深度；near/far 只负责光栅裁剪。
4. cube 只有 `validFaceMask == 0x3F` 才 ready。任一面缺失、资源重建、灯光重排或签名变化都 fail-soft 为直接光，不能跨 seam 读取未初始化 layer。
5. 16-tap manual PCF 使用 nearest cube sampler和严格零质心同心盘，避免双重过滤及方向偏置。
6. shadow term 在自身 `currentDist / shadowRange` 域于范围边缘平滑淡回 1；直接光本身还有平方 soft window，不产生新增硬截断。
7. cube array 受 96 MiB D32 预算约束；分辨率提高时自动降低可投影灯容量，其余灯仍保留直接照明。
8. 质量默认不再把每个 face 的 known caster 静默截到 192 个；
   `pointShadowMaxCastersPerFace=0` 表示 unlimited。显式性能 cap 仍保留
   unknown-bounds pinned + known-caster 近距排序，并逐 face 记录
   `candidate/kept/dropped`、低频输出 cap-hit，避免把缺失 caster 误判为 cube 坐标错位。
9. snapshot producer 会排除非有限位置/range/color、零 range、零 intensity 和
   `maxRGB=0` 的黑灯；importance 使用 finite double score。receiver 仍消费同一
   canonical generation/order，但 view-space light position 已由 CPU 每帧/灯写入
   既有 `params.yzw`，没有扩大 48-byte/light 或 784-byte UBO ABI。以
   2560x1440、16 灯为上界，理论上最多移除约 5900 万次 shader position transform/
   帧；这是静态操作数估计，不是实测 GPU 时间。
10. cube face VP 已对齐仓库 `Matrix4` 的反向 raw-product 语义：C++ 写
    `proj * view`，row-vector shader 才实际收到 raw `view * proj`。旧的
    `view * proj` 会让光源平移后连 face 正前方 caster 都落到视锥外，是“阴影位置
    不对”的确定性根因；clip-X cube convention 与六面 dir/up 不变。
11. 默认六面/每帧质量档不再先做无效的 O(caster+palette) 内容签名；只有 temporal
    reuse 或 1..5 face 轮转需要历史时才保留完整签名与 dynamic 全六面门。24 个
    face caster vector 用 clear 保留 capacity；unlimited 档直接按 replay 顺序追加，
    跳过逐 caster sqrt、partition 和 sort。range 与 direct-light outside reject 改为
    squared distance；cube PCF 去掉 16 个数学冗余 normalize（单 mip cube 方向不变）。
    可选历史签名已覆盖 dynamic skinned output、position/index/blend/UV 的 layout 与
    slice、draw range、alpha/texture/sampler；默认六面路径仍不进入该 O(caster+palette)
    循环。
12. point cube 在 CSM disabled/reused 时可能成为 GPU-skin/frozen geometry 的首个
    replay consumer，因此在 draw 前显式执行 `transfer|compute write -> vertex|index
    read` memory barrier，不再依赖 CSM 恰好先消费。
13. PCF 与 texel bias 使用方向相关的保守 cube footprint
    `2*max(abs(normalizedDirection))/resolution`；中心精确，edge/corner 不再沿用中心
    texel 尺度。环境 float 拒绝 NaN/Inf，consumer 端仍做 finite default fallback。
14. 发布身份已从 0..2 的资源 `frameIndex` 拆成单调 64-bit `frameSerial`：light
    snapshot、camera、ReplayDraw、point cube 和 CSM settlement 使用 serial；point cube
    只允许本帧真实发布或显式 temporal reuse，CSM 只允许本帧 render 成功或显式 reuse。
    Volume 以 expected serial exact gate 两类 snapshot；Shadow 抛异常或资源缺失的
    non-throwing false 都不能把旧 cube/CSM 重标为当前帧。camera 只允许当前或前一 serial，
    静态复核 P0/P1=0。
15. 点光 receiver 不再从 early-return 后的 depth-reconstructed position 使用
    `dFdx/dFdy` 推法线。模型轮廓现在显式读取 L/R/U/D 最多 4 个 integer depth texel，
    拒绝 clear、跨 surface 和不可信深度跳变；只从同一 surface 的一侧 tangent 重建法线。
    `viewFacing` 只用于翻转合法法线半球，不再混入着色。缺轴/低 confidence 时点光增量
    平滑归零，保留底图而不是把薄片/角点照成锯齿亮边；同一 gated normal 同时服务
    Lambert、cube slope bias 与 contact ray。

独立 UV binding 的静态 P1 已闭合：draw、persistent geometry 与 pipeline key 显式
传播 `uvBinding=0/1/2`（position/blend/separate），CSM、terrain mask、point cube 与
两条 outline consumer 均按 owner binding 绑定并跟踪资源。raw legacy 在 persistent
probe 前解析真实 stream；动态 separate UV 只走逐帧冻结 fallback，静态 separate UV
进入 source hash。semantic alpha payload 在首个 cache probe 前进入 UV/纹理 key，
stash 使用真实 upload stride 并校验 offset/元素宽度。S1 early cache 读与两个写点均
限制为 opaque，alpha draw 不再污染无材质 key。静态终审 P0/P1=0；这仍不是画质通过，
必须在运行 gate 覆盖 binding1/2 树叶、栅栏、point/CSM 与 outline。

当前默认：

| 参数 | 值 |
|---|---:|
| `pointShadowMaxLights` | 1 |
| `pointShadowResolution` | 1024 |
| 可选分辨率 | 128 / 256 / 512 / 1024 / 2048 |
| `pointShadowTemporalReuse` | false |
| `pointShadowUpdatePeriod` | 1 |
| `pointShadowMaxFacesPerFrame` | 6（同帧完整 cube） |
| `pointShadowPcfRadiusNear/Far` | 0.65 / 1.15 |
| `pointShadowRangeFadeStart` | 0.78 |
| `pointShadowMaxCastersPerFace` | 0（unlimited，质量默认） |

动态蒙皮 caster 会使内容签名变化，通常要求六面同帧更新。这是正确性优先的高成本路径，不能用轮转 face 冻结 pose 来换伪 FPS。

## 体积光当前实现

渲染分两段：

1. 低分辨率 `R16G16B16A16_SFLOAT` effect：
   - `rgb` 为沿 ray 积分的散射；
   - `a` 为该 ray 的 Beer-Lambert transmittance。
2. full-resolution composite：
   - 用 full-resolution depth 引导邻近 effect rays 的空间重建；
   - 合成 `base * T + softHeadroom(scattering)`；
   - 无效果像素 exact passthrough。

2026-07-14 修正的根因：

- 低分辨率 effect 不再线性过滤非线性 hardware depth 后反投影；每个 effect pixel 使用对应 full-resolution 整数 depth texel，并对齐 world reconstruction。
- depth reconstruction 复用 A1/receiver 的共享契约：保留 signed
  `MaxZ-MinZ`，由 projection 推导 far clear raw，并按 D16/D24/D32 quantum
  判断 clear。正常/反向 viewport 均选择对应的 `0.999/0.001` far endpoint；
  合同无效时在资源申请和 color/depth copy 前 fail-soft。
- `Run()` 保留 `HasActiveLights()` 无锁快门，但它只表示“需要检查”。在任何资源申请
  和 copy 前仅取得一次真实 immutable snapshot；canonical 已过滤零能量/非法灯，
  volume consumer 再用精确最大 march 光程与共享保守 sphere/viewport 投影拒绝
  不可能影响任一当前 view ray 的灯。point-only 且最终候选为 0 时，两次
  full-resolution copy、effect pass 与 composite pass 全部跳过；sun-only 不受影响。
- `density` 在 CPU 使用与 shader 完全相同的 `clamp(0..2, fallback=0)` no-effect
  gate；零值或非有限值不再进入空跑 fullscreen 流程。
- `sun.enabled=false` 不再注入强度 1；太阳与点光散射完全独立。
- 太阳源 gate 使用真实 `maxRGB * intensity`；黑色/NaN/零辐射太阳不再触发全屏
  pass 或无源 extinction。太阳和点光统一以 `color * sourceIntensity` 进入散射，
  HDR 颜色保持单调响应但仍受唯一 `softClipRgb(..., 0.92)` 与 composite headroom
  约束。
- `requireCsmSnapshot=true` 且 CSM 缺失时，仅太阳散射 fail-soft；合法点光 volume 仍可运行。缺 CSM 时绑定维度匹配的 placeholder，`cascadeCount=0`。
- optional CSM 缺失时，`unshadowedScattering` 始终是太阳 fallback 的权威值；它不再
  被 `shadowStrength=0` 绕过而凭空生成 100% 无阴影太阳散射。有真实 CSM 时
  `shadowStrength=0` 仍保持全亮，两个设置语义彼此独立。
- CSM UV/Z coverage 由二值切换改为 feather；有 CSM 时从 fallback 平滑过渡到 shadow visibility，`shadowStrength=0` 保证全亮。
- `decay` 改映射单次散射反照率；不再错误地降低 extinction，解决“decay 拉高反而更看不见”。
- 每步使用 `T=exp(-sigmaT*ds)` 和 `L=(1-T)*(sigmaS/sigmaT)`；HG phase 归一到 LDR 光照单位，移除旧魔法倍率。
- `samplePos` 的 view-Z 是沿 view ray 的仿射函数；producer 现在把 camera intercept
  与 ray slope 提到 4..16 步循环外，循环内只做一次 multiply-add。CSM 的 4 个
  `cameraClip/rayClipSlope` 也每像素预计算，固定 8 次矩阵变换，longitudinal probe
  只做 vec4 FMA；最坏不再为每像素重复 128 次 world-to-light transform。
- 高度雾使用有界 signed exponential profile。
- 点光体积散射与表面 receiver 使用同一 range-normalized 能量合同：
  `(1-x²)² / (1+6x²)`。这移除了隐藏的 `0.1R` 发光核心和 raw inverse-square
  在常见 `0.5R..0.8R` 距离造成约 5–7 倍能量损失的问题，同时在范围端连续归零。
- 每条 view ray 现在为最多 2 盏点光各计算一次解析 ray-sphere
  `[enter, exit]`，并裁到当前表面与 `maxWorldDistance`。每个 march segment 只在
  与该区间有 overlap 时评估点光 attenuation/HG，再用该 overlap 的真实光学长度
  积分 Beer-Lambert。细小或掠射 chord 不再依赖抖动 midpoint 恰好落入球内；无
  overlap 的灯跳过逐步 `length`。`maxOpticalDepth` 截断后的段尾不会继续散射。
- 太阳 shadow visibility 把旧的“单一纵向点 × 最多 8 个空间 PCF tap”重排为
  “每段最多 8 个纵向 raw-depth probe”。raw compare 使用 `texelFetch`，不受 active
  Linear CSM sampler 插值污染；每个 probe 内分别得到 reference occlusion
  `(1-coverage)*(1-fallback)` 与 physical occlusion
  `reference+coverage*(1-visibility)`，两者各自在段内取 max，不能把不同 probe 的
  coverage-edge 与 blocker 双重相加。
- 用户实测已能看到阴影柱，但上帝视角仍弱且需要过高 intensity。已证明的首要旧因不是
  单纯 probe 数：俯视 ray 往往只横穿 1/16 段，旧 `weight` 在那个二值段内已经饱和，
  无法继续扩大整条 ray 的明暗差；旧 HG 映射又让低镜头 forward scatter 天然更亮。
  后续复核同时确认固定纵向间距与 quarter-resolution 重建仍会放大残余视角差，不能把
  “首要旧因”误写成“采样已无问题”。
- 太阳现为 reference 与 physical visibility 分别累计整条 ray，令真实遮挡占比
  `O=1-clamp(Aphysical/Aref,0,1)`。`weight=0..1` 使用 `Oout=O*weight`，所以 0 精确去掉
  CSM 对比、1 精确恢复物理积分。用户在 DLL `5EECB...93B`、weight=3 的 2048x1105 RTS
  截图中仍无法辨认阴影柱，证明旧 `pow(physicalRatio,weight)` 对 `O≈1/64` 的短柱只有约
  3.86% 对比，不能过门。最新 `weight>1` 改为仅对真实 `O` 使用 bounded readability toe：
  低镜头目标指数 0.68，俯视目标指数 0.50，并随 weight 从物理指数 1 平滑进入。相同
  `O=1/64` 在最大档为低镜头 5.91%、俯视 12.5%；`O=0` 仍严格映射 0，因此无 CSM、
  strength=0 或 coverage-only fallback 不能凭空造柱。
- 2026-07-16 的第三阶段把“随镜头压低从物体长向地面”拆成两个空间缺口并分别闭合：
  volume 终于消费 C++ 已上传的 `cascadeBlendRange`，在 split 过渡带同时采相邻级联；
  它先混合 `coverage*(1-visibility)` 的真实遮挡量和 coverage，再重建 visibility，不能因
  coverage/visibility 的错误交叉项造伪柱。主级联投影无效时与 surface receiver 一样直接
  fail-soft；不能在 boolean projection edge 突然切到 coverage≈1 的粗级联。普通 probe 仍是
  一次 raw fetch，只有连续的级联 split 过渡带最坏两次。
- 同一阶段为 post-FX 与 volume 都 enabled 的 C2/C3 增加固定 384 world-unit、
  只向太阳侧的 caster depth extension。公式为 `baseD=radius+margin`、`eye=baseD+E`、
  `maxZ=2*baseD+E`，因此远 receiver 边界不变，Z 范围只增加 E 而不是对称增加 2E；C0/C1
  与关闭体积光时的 CSM 保持原合同。它解决 caster 位于 receiver sphere 上游时的软件提交后
  仍被 projection near-Z 裁掉的问题，同时避免历史 `+3000` 对称扩张造成的精度/抖动回归。
- 少散射仍是主体，但它对低能量、短占比的俯视柱存在数学上限。因此 `weight>1` 现在允许将
  resolved true CSM occlusion 复用到已有 alpha 透过率通道：额外底图衰减为
  `min(Oresolved*0.45*mediumGate*sourceGate*readabilityMix, 0.18)`。`weight<=1` 的物理合同不变；
  无真实 blocker、极低光学厚度、极弱太阳或关闭体积光时该项严格归零，不能留下脱离雾层的
  黑贴花。离线边界在 gates=1 时，`O=1/64`、最大档增加 5.625% 底图对比，`O=1/16`
  增加 11.25%，完整遮挡也硬封顶 18%。
- HG 方向性从 `g=clamp((skyThreshold-.5)*.9,0,.55)` 收窄为
  `g=clamp((skyThreshold-.55)*.50,0,.22)`；最新 HG/iso mix 继续从 0.55 收窄到 0.35，
  归一化 phase floor 为 0.80（当前最坏 backscatter 自然值 0.833，不触发增能 clamp）。
  离线边界为 forward/back 响应比 `2.104 -> 1.622`；这是把能量从低镜头 forward 峰重新分配到常见 RTS 俯视的
  side/back，不是提高全局雾强度。
- 太阳纵向 probe 的每段硬上限仍为 8，但目标间距不再固定 24 world units：除
  `abs(dot(viewRay,sunDir))` 的横穿分量外，`abs(dot(viewRay,worldUp))` 的 RTS 俯视因子也会
  在 `24..10` 间收紧。俯视的屏幕柱 footprint 最短且 quarter-res 独立 ray 更少，不能因其
  近似平行 sun direction 而退回旧 24 spacing；segment、matrix 与 TDR 上限仍未放宽。
- 点光的平滑能量仍在 overlap midpoint 计算，但 cube visibility 在 overlap 内使用
  2 个分层 probe 并取更暗值，减少单位尺寸点阴影锥落在两个 march midpoint 之间的漏检。
  CPU draw、shader count 和 interval array 均硬限 2 盏灯，所以每段最多 4 次 cube fetch。
- point-only 像素若视线与所有光球都不相交仍 exact passthrough；这是局部受光体积
  的产品语义，不是全屏均匀雾。极弱散射阈值处的 alpha 连续性仅记为 P2，未在本批
  扩张语义。
- point-only、无太阳且显式 `resolutionDivisor=2` 时，最终 top-K 光球复用 A1 的
  ceil-half 保守投影并合并 union ROI。当前安全默认 divisor=4 走保守 full-effect
  extent，由 4M segment 门兜底。divisor=2 路径的 effect 仍以 full renderArea/CLEAR 每帧把 ROI
  外清成 `(0,0,0,1)`，只缩 draw scissor；composite 按真实 full/effect odd-size 比率
  扩 guard 后缩 scissor，并以 `LOAD` 保留 ROI 外原画面。其他 divisor 与任何太阳路径
  仍走原全屏流程。当前 v1 仍保留 full color/depth copy 与 attachment fast-clear。
- composite 把 alpha 明确定义为每条 ray 的 resolved base transmittance（物理 T 加上上述
  true-CSM-only bounded readability），空间上做线性重建；散射使用按通道 headroom 的指数
  soft shoulder，最大档可见但不会硬冲白。
  最新重建先在 raw-depth 相容候选中选屏幕空间最近的 reference，再以相对散射差做第二个
  bilateral 权重。这样平坦地形上没有 depth edge 的体积阴影边界不会被 2x2 low-res 邻域
  直接冲淡；小于 8% 的平滑散射梯度仍保持原双线性行为，强边界保留 1/64 有界串扰以避免
  完全断开的方块边。该修改不能恢复四条 low-res ray 都没采中的柱，因此必须与上面的
  longitudinal admission 一起验收。
- 全局 light snapshot 必须继续保持 shadow-first canonical 顺序，因为 cube、contact
  与 receiver 共享该索引合同；体积光不能再把这一前缀误当成自己的重要度排序。
  体积消费者现在只在栈上建立最多 16 个候选索引，以
  `clamped maxRGB * intensity * range² / (1 + cameraDist²)` 选择配置预算内的
  stable top-K（执行硬上限 2），再按所选索引填自己的 UBO。score 非有限或负数归零，同分用
  canonical index 决胜，无堆分配；当预算不少于可用灯数时完全保持原序。
  因此远处/暗的投影阴影灯不能再把近处明亮非阴影灯从默认 2 灯体积预算中
  挤掉。K/K+1 在相机移动时交叉仍可能发生一次整灯切换，列为 P2 运行画质门；
  无实机证据前不引入跨帧 hysteresis/reset 状态。
  在 local top-K 之前还会拒绝 `distance(camera,center)-range > maxWorldDistance`
  以及保守投影证明 wholly-behind/offscreen 的光球；camera-plane/near-plane crossing、
  非法投影和边界接触全部 fail-soft 保留。筛选结果只保存 canonical index，不修改
  shared snapshot、shadow prefix、cube/contact layer 或 generation。

当前默认：

| 参数 | 值 |
|---|---:|
| enabled | false |
| intensity | 0.65 |
| decay | 0.95 |
| density | 0.70 |
| weight | 1.25 |
| samples | 16 |
| resolutionDivisor | 4 |
| sunDistance | 1400 |
| extinctionStrength | 0.18 |
| unshadowedScattering | 0.22 |
| requireCsmSnapshot | true |
| includePointLights | true |
| maxPointLights | 2 |

`volumetric_max_quality` 是画质/可见性相干 preset，不是把每个滑杆拉满。2026-07-14
俯视门使用 intensity=1.5、density=2.0、weight=2.25、heightFogStrength=0.50、
fadeNear=0、fadeFar=0.55、extinction=0.08、divisor=4、samples=16；旧
height=2/fadeFar=1 会把介质压向地表并让短 ray 更晚显现，正好强化“只能低镜头看到”。

上述上帝视角修正已通过 shader 生成与 x86 build-only，artifact：
`AutoTest/artifacts/gpu_skin_timing_volume_build_only_20260714_132000`，DLL SHA256
`13927E439386E1837343632914DA4D919DB56145FCD7B95991CBC55C4EB3742B`；尚未部署或运行
War3，仍需隔离桌面做 CSM coverage edge/俯视与低镜头 A/B，不能据 build-only 宣称画质门通过。

2026-07-16 第三阶段最终 build/deploy：两条 GLSL 生成成功，x86 增量 4/4 compile/link、
exit 0，随后 ninja no-work；DLL 29,921,590 bytes，SHA256
`9A795542BFD01FB55B85EC5380EB37A45A6558B38B3F8587F8A4641A51340201`。在 War3、
World Editor、YDWE/ydhost 进程均为 0 时部署到 `E:\Work\War3\d3d9.dll`，source/target hash
exact match。本阶段未启动 War3、未运行 AutoTest；Python 算术证据在
`AutoTest/artifacts/volumetric_camera_continuity_offline_20260716/result.json`，它证明公式与
预算闭合，不证明真实截图已过门。

关闭、零 intensity、零 density、非法 depth contract，以及 point-only 无真实可见候选
路径均在任何 effect 资源申请、color/depth copy 之前 return。

## 环境变量

点光与 cube 阴影：

- `DXVK_WAR3_POINT_LIGHTS=0/1`
- `DXVK_WAR3_POINT_SHADOW=0/1`
- `DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS=1..4`
- `DXVK_WAR3_POINT_SHADOW_RESOLUTION=128..2048`
- `DXVK_WAR3_POINT_SHADOW_BIAS=<float>`
- `DXVK_WAR3_POINT_SHADOW_PCF_NEAR=<float>`
- `DXVK_WAR3_POINT_SHADOW_PCF_FAR=<float>`
- `DXVK_WAR3_POINT_SHADOW_TEXEL_BIAS=<float>`
- `DXVK_WAR3_POINT_SHADOW_RANGE_FADE=<0.50..0.98>`
- `DXVK_WAR3_POINT_SHADOW_MAX_FACES=0..6`
- `DXVK_WAR3_TEST_POINT_LIGHT=1`
- `DXVK_WAR3_TEST_POINT_LIGHT_CLEAR=1`
- `DXVK_WAR3_TEST_POINT_LIGHT_X/Y/Z=<float>`
- `DXVK_WAR3_TEST_POINT_LIGHT_RANGE=<float>`
- `DXVK_WAR3_TEST_POINT_LIGHT_R/G/B=<float>`
- `DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY=<float>`
- `DXVK_WAR3_TEST_POINT_LIGHT_SHADOW=<0..1>`

软件 contact ray：

- `DXVK_WAR3_POINT_RAY_SHADOW=0/1`
- `DXVK_WAR3_POINT_RAY_SHADOW_HIZ=0/1`
- `DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS=1..2`
- `DXVK_WAR3_POINT_RAY_SHADOW_STEPS=4..32`（A0）
- `DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS=8..64`（A1）
- `DXVK_WAR3_POINT_RAY_SHADOW_MAX_DISTANCE=<32..2400>`
- `DXVK_WAR3_POINT_RAY_SHADOW_THICKNESS=<1..160>`
- `DXVK_WAR3_POINT_RAY_SHADOW_START_OFFSET=<1..96>`
- `DXVK_WAR3_POINT_RAY_SHADOW_STRENGTH=<0..1>`

体积光：

- `DXVK_WAR3_VOLUMETRIC_LIGHT=0/1`
- `DXVK_WAR3_VOLUMETRIC_RES_DIVISOR=4..8`
- `DXVK_WAR3_VOLUMETRIC_INTENSITY=<float>`
- `DXVK_WAR3_VOLUMETRIC_DENSITY=<float>`
- `DXVK_WAR3_VOLUMETRIC_WEIGHT=<float>`
- `DXVK_WAR3_VOLUMETRIC_DECAY=<0.70..0.999>`
- `DXVK_WAR3_VOLUMETRIC_SAMPLES=4..16`
- `DXVK_WAR3_VOLUMETRIC_POINT_MAX_LIGHTS=0..2`
- `DXVK_WAR3_VOLUMETRIC_FADE_NEAR=<float>`
- `DXVK_WAR3_VOLUMETRIC_FADE_FAR=<float>`
- `DXVK_WAR3_VOLUMETRIC_MAX_RAY=<float>`
- `DXVK_WAR3_VOLUMETRIC_HEIGHT_FOG=<float>`
- `DXVK_WAR3_VOLUMETRIC_EXTINCTION=<0..1>`
- `DXVK_WAR3_VOLUMETRIC_UNSHADOWED=<0..1>`

## JASS Command Bridge

点光：

- `add-point-light|x|y|z|range|r|g|b|intensity|shadowIntensity`
- `update-point-light|id|x|y|z|range|r|g|b|intensity`
- `update-point-light-ex|id|x|y|z|range|r|g|b|intensity|shadowIntensity`
- `set-point-light-shadow|id|shadowIntensity`
- `remove-point-light|id`
- `clear-point-lights`
- `get-point-light-count`
- `set-point-lights-enabled|true/false`
- `set-point-shadow-enabled|true/false`
- `set-point-shadow-bias|bias`

体积光：

- `set-volumetric-light-enabled|true/false`
- `set-volumetric-enabled|true/false`
- `set-volumetric-light-params|intensity|density|weight|decay|samples`
- `set-volumetric-light-fade|fadeNear|fadeFar|maxRayDistance`
- `set-volumetric-height-fog|baseHeight|falloff|strength`
- `set-volumetric-resolution-divisor|4..8`

## 2026-07-14 低镜头未响应：GPU TDR 根因与硬预算

用户两次在开启体积光、压低镜头后遇到未响应。精确进程证据中，War3 PID `33988`
启动于 08:28:12；08:29:21 出现 `nvlddmkm Event 153 (GPUID 900)`，同秒
`war3_d3d9.log` 记录 `VK_ERROR_DEVICE_LOST`。另一次 War3 PID `11364` 有
Application Hang 1002。该模式符合 GPU watchdog/TDR，而不是 CPU mutex 或蒙皮线程死锁。

旧 2560x1351 默认 divisor=2、32 samples 在低镜头 fail-soft 到 full effect extent 时，
单帧约 `27,688,960` ray segments；8-tap CSM 是 `221,511,680` 次潜在 fetch，cascade
blend 可到两倍，4 盏点光又增加约 `110,755,840` 次内循环。旧暴露上限
divisor=1、96 samples、8 lights 可达到 `332,021,760` segments 和十亿级采样，足以触发
TDR。

当前安全合同：

- 默认启用档为 divisor=4、samples=16、point lights=2；env、Shader API、ImGui、pass
  execution 和 fragment shader 均有一致 clamp；
- `kVolumetricRaySegmentBudget=4,000,000`。先按 effect pixels 收缩 sample count；若最低
  4 samples 仍超预算，自动把 divisor 提高到最多 8；divisor=8 仍超预算时跳过 optional
  volume pass，不能用 quality floor 偷穿 watchdog 上限；
- shader 循环静态上限 16，外部旧配置无法恢复 96-step 路径；
- 2560x1351 安全档为 640x338、16 samples，即 `3,461,120` segments，约为旧默认的
  `1/8`、旧暴露最大值的 `1/95.9`。
- 每 segment 最多 8 个 longitudinal CSM probe；普通区域为 8 次 raw fetch，级联 seam
  最坏因相邻级联 blend 为 16 次。再加 2 lights × 2 cube probe，普通/绝对最坏组合上界
  分别为 12/20 fetch per segment。因此 4M segment 门对应约 4800 万普通上界、8000 万
  seam-only 绝对上界；真实 seam 只占 split 过渡带。2560x1351 安全档为约 2769 万普通
  CSM（seam-only 最坏 5538 万）+ 1384 万 cube fetch，且 no-CSM/strength0 跳 CSM、
  weight0 跳 cube。

更新后的纯算术模型位于
`AutoTest/artifacts/volumetric_offline_cost_20260714_122642/result.json`。它不会启动或
附加 War3；这些只是执行上界，不是 GPU 用时或画质通过结论。

体积光“不明显”与 TDR 是两个问题。sample count 只控制积分精度，`ds` 会按段数补偿；
可见度应通过 coherent intensity/density/weight、sun/point source energy 与 fade 合同调节，
不能再提高 loop bound。“全部滑杆拉满”也不是最大可见 preset：fadeNear、minSunIntensity、
extinction 属于门限/抑制参数。安全门运行通过后再做能量 preset 的画质验收。

## 历史运行证据与当前验收边界

2026-07-09 的 `光影测试.w3x` 短窗口曾证明旧入口可运行：

| 历史场景 | 历史 avgFps |
|---|---:|
| baseline | 51.247 |
| 点光源 + 点阴影 | 42.579 |
| 旧体积光 full/half-res | 40.952 |
| 点光源 + 旧体积光 divisor=2 | 48.437 |
| 点光源 + 旧体积光 divisor=4 | 51.058 |

这些数字对应旧 shader、旧默认值和旧测试环境，只能证明入口历史上能运行，不能作为 2026-07-14 算法的性能或画质结论。

后续严格顺序：

1. build-only 已完成；不得将其解释为运行画质或性能通过。
2. Warcraft III 可用后，由唯一 Test Conductor 在隔离桌面只跑
   `光影测试.w3x` 的短 crash/画质 gate：
   - 点光位置、六个 cube seam、范围边缘，以及 UV binding1/2 树叶/栅栏在
     CSM、point cube 与 outline 的 alpha 形状；
   - 体积光 sun-only、point-only、CSM missing、最大档；
   - 至少 3 盏重叠异色点光、`maxPointLights=2`，缓慢移动相机穿过 K/K+1
     交点，检查漏选与一次性 selection pop；
   - A0/A1 对照、深度断层、reversed depth、resize/reset 和第二进程。
3. isolated desktop 不判 FPS。
4. 用户明确让出前台后，最后才跑 foreground `dual_perf` 和特性 A/B。

`run_light_feature_matrix.py` 现有 8 组配置，点阴影分别覆盖 512 性能对照、1024
产品默认与 2048 Ultra；三者都要求 exact PID 的实际 point-shadow 执行证据。体积光
最大可见验收必须使用相干 preset，不能把 fade、minSunIntensity、extinction 等
抑制/门限参数机械地全部拉满。

S1 terrain capture period 必须保持 1；本专项不修改点光、体积光之外的 CSM/S1 算法。
