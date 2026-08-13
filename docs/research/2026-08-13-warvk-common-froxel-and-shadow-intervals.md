# WarVK 统一 Froxel 与连续太阳阴影区间

日期：2026-08-13

## 结论

逐屏幕列、贴地表的 `[L-D,L]` 深度域不是合法 Froxel 网格。当地表距离 `L`
因俯仰变化时，网格近端也随之移动，所以 caster 附近的介质会被逐渐裁掉；更严重的
是，同一个整数 `(x,y,z)` 的 `z` 在相邻 XY 列对应不同世界距离，3D 纹理的空间过滤
和历史重投影因而会混合无关位置。这分别对应玩家观察到的“越压低视角阴影越短”和
“许多 caster 轮廓叠在一起”。

当前候选恢复全屏统一的相机射线深度分布，并把高频太阳可见度从低分辨率 Froxel
辐亮度场中拆出。Froxel 只存储介质消光与低频点光源项；太阳阴影在最终射线积分时
按 shadow-map texel 区间求覆盖率。场景深度只终止积分，不再平移或缩放网格。

## 一手依据

1. Bart Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to
   Atmospheric Scattering*, SIGGRAPH 2014。该方案把介质放入统一的视锥 3D 网格，讨论
   指数深度分布、低分辨率走样、抖动与历史重投影的取舍。
   https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf
2. Jiawen Chen 等，*Real-time Volumetric Shadows using 1D Min-Max Mipmaps*,
   I3D 2011。论文把相机 ray 与 shadow-map row 对齐，并用 1D min-max 层次寻找连续
   lit segments，说明高频阴影应作为沿射线的区间问题处理，而不是复制进低分辨率
   体素层。
   https://groups.csail.mit.edu/graphics/mmvs/mmvs.pdf
3. Thomas Engelhardt, Carsten Dachsbacher, *Epipolar Sampling for Shadows and
   Crepuscular Rays in Participating Media with Single Scattering*, I3D 2010。
   该工作以 epipolar 几何复用沿射线的可见度采样。
   https://publikationen.bibliothek.kit.edu/1000026862
4. Reeves、Salesin、Cook，*Rendering Antialiased Shadows with Depth Maps*,
   SIGGRAPH 1987。PCF 的对象是深度比较结果；不能先把原始深度平均后只比较一次。
   https://dl.acm.org/doi/10.1145/37402.37435
5. Khronos Vulkan Guide, *Synchronization Examples*。深度附件写入转 shader 读取、
   compute 写后读都必须在实际 producer/consumer stage 与 access 上建立依赖。
   https://docs.vulkan.org/guide/latest/synchronization_examples.html
6. Microsoft, *Common Techniques to Improve Shadow Depth Maps*。官方文档明确将过大
   depth offset 导致的阴影脱离称为 Peter Panning，并要求尽量收紧光空间 near/far；
   这直接授权本轮把体积太阳 bias 从固定归一化数改为固定世界尺度。
   https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps
7. Johannes Kopf、Michael F. Cohen、Dani Lischinski、Matt Uyttendaele，
   *Joint Bilateral Upsampling*, ACM TOG / SIGGRAPH 2007。论文将低分辨率解用空间核重建，
   并以独立的高分辨率输入作为 range guide；这授权本轮由全分辨率 scene depth 保护真正
   的几何断层，而不是让低分辨率阴影透射率自己成为 range guide。
   https://johanneskopf.de/publications/jbu/paper/FinalPaper_0185.pdf
8. Khronos, *The OpenGL Shading Language 4.60*, §8.9.4 Texture Gather
   Functions。规范定义 `textureGather` 用一次操作取得 LINEAR footprint 的四个 texel，顺序为
   `(i0,j1),(i1,j1),(i1,j0),(i0,j0)`；shadow sampler 语义也是分别比较四个 texel，而不是
   先平均深度。WarVK 使用非 comparison sampler 取得四个原始深度，再分别执行解析区间比较，
   数学上仍严格遵循 Reeves PCF 的“先比较、后过滤”。
   https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#texture-gather-functions

## 统一坐标合同

所有 XY 列共用：

```text
z(k) = n * exp(log(f/n) * k/N),  k = 0..N
```

## 体积太阳偏移与回退相位修正

体积太阳 ortho 是线性深度，故世界空间偏移与比较深度偏移满足：

```text
biasNormalized = biasWorld / (maxZ - minZ)
```

旧值把 `0.0075` 直接当归一化深度；在当前 10,000 世界单位 far 半径下，
`maxZ-minZ` 约为 20,000，等价于把阴影起点推开约 150 世界单位。近层也约为
70 世界单位。这不是精度保护，而是可见的 Peter Panning，且低俯角会把这段距离
投影成 Caster 与阴影柱之间的长亮缝。当前实现把作者值定义为世界单位（默认 2），
再按 far 层深度跨度归一化；near 层沿用该归一化值，因此实际世界偏移只会更小。

shadow-texel DDA 预算耗尽后的密集回退仍保持 32 世界单位上限，但采样边界统一锚定
到共同 `froxelNear`。它不再在每个 64/128 Z slice 内重新等分，从数值结构上消除
“每层重新投影一次 Caster 轮廓”的相位来源。该修正不改变表面 CSM、阴影生产几何
或介质密度。

- `n=20` 为共同 near；
- `f=10000` 为共同 far；旧 JASS 的 legacy distance 不再改变该值；
- Medium / High 分别为 `N=64 / 128`；
- `z(k)` 不读取 scene depth，也不依赖屏幕 XY。

对某像素重建表面距离 `L` 后，仅令：

```text
t_max = min(L, f)       // 有实体表面
t_max = f               // 天空
```

积分遍历固定 `z(k)`，并把最后一段截到 `t_max`。因此俯仰变化会改变可见表面和射线，
但不会从 caster 一端移动整个体积域。统一坐标也恢复了正确的世界位置历史重投影：
世界点先投到上一帧 UV，再按上一帧相机距离映回同一对数 Z。

旧 JASS `VolumetricSetQuality(samples,maxDistance)` 的第二个参数原本只属于 RayMarch。
为兼容常见的 1200–1800 地图脚本，它继续设置 legacy distance；Froxel far 只允许该
参数不会再改变 Froxel far，避免相同地图因调用顺序重新缩短公共域。

3D grid XY 和最终 effect XY 现在具有不同职责。Medium/High grid tile 仍为
`32/16`，但 effect divisor 为 `8/4`；High 在 4K 会由主 DDA admission budget
自动降为 `1/8`。场景深度终止发生在更细的 effect 像素上，不能再由一个
16×16 或 32×32 tile 共用首个表面判断。

## 太阳阴影区间积分

太阳辐亮度不再在 Inject 阶段乘进每个 Froxel。对于积分段 `[a,b]`，方向光正交矩阵
使 shadow 坐标与参考深度沿相机 ray 都是仿射函数：

```text
u(t) = u0 + du*t
v(t) = v0 + dv*t
r(t) = r0 + dr*t - bias
```

候选在 shadow texel 的整数边界执行 2D DDA。在一个 texel 内，存储深度 `d` 为常数；
可见函数为 `r(t) <= d`。若端点比较结果不同，交点比例可解析求得：

```text
q = clamp((d-r0)/(r1-r0), 0, 1)
litFraction = lit_at_start ? q : 1-q
```

该比例按世界段长加权，再与 `sigma_t`、single-scattering albedo 和
Henyey-Greenstein phase 一起进入 Beer-Lambert 闭式积分。这样 caster 边界是 shadow
texel 上的连续区间交点，而不是在 64/128 个 Froxel 层里复制 64/128 次轮廓。

lit fraction 采用光学厚度加权。令
`I(x)=(1-exp(-sigma_t*x))/sigma_t`；若一个区间先暗后亮，亮区贡献为
`I(length)-I(crossing)`，不能只按几何长度比例平均后再进行 Beer-Lambert
积分。只有阴影深度交界附近才启用四点 comparison average。

## 清澈介质中的阴影柱可读性

玩家物理回归确认统一网格消除了低视角收缩和多重 caster 轮廓，但首版 Froxel 只把
CSM 可见度乘入太阳散射。当地图库刻意使用很低的介质能量时，“有光区少量散射、遮挡区
没有散射”的差值会小到几乎不可见；旧 RayMarch 路径还包含一项受真实 CSM 证据约束的
背景透射率衰减，Froxel 漏掉了该输出合同。

候选为每个通过 `selectSegmentCascade` 完整覆盖证明的解析区间保留两个量。令区间的
原始 shadow-map 可见度为 `V`，作者阴影强度为 `s`，则物理可见度为：

```text
V_physical = mix(1, V, s)
S_ref      += T_camera * sigma_t * I(length)
S_physical += T_camera * sigma_t * I(length) * V_physical
E_shadow   += sigma_t * length * (1 - V_physical)
O_peak      = max(O_peak, 1 - V_physical)
```

`contrast` 的幂只继续作用于太阳散射显示，不进入上述物理证据，避免作者对比度自己制造
“遮挡事实”。无法选择有效 CSM 级联的区间只使用既有 fallback 散射，不累计 `S_ref`、
`E_shadow` 或 `O_peak`。因此缺失覆盖、无 shadow map 或 CSM 投影外区域都不能产生暗柱。

整条射线结束后：

```text
O_path = 1 - S_physical / S_ref
G      = smoothstep(0.00035, 0.0060, E_shadow)
O      = max(O_path, O_peak * G * localScale)
```

随后沿用旧路径的固定对比度解析，并仅在 `contrast > 1`、太阳辐亮度门和证据门同时成立时
压低现有 base-transmittance。额外衰减固定上限为 24%，不改变 Froxel 密度、全局介质或
表面 CSM。`0.00035/0.0060`、`0.68/0.74`、指数目标与 24% 都是 Warcraft III 的美术
候选常数，不来自 Wronski、Chen 或 Engelhardt 论文；论文只授权统一介质积分与真实阴影
可见度的物理结构。它们仍必须通过同一 caster、同一太阳、俯视到最低角的玩家前台 A/B，
并检查关闭体积光后的表面 CSM 完全不变。

首轮可读性回归暴露出 composite 的另一项错误：RGB 与 alpha 共用由低分辨率 effect 自身
强边生成的 range weight，最低跨边权重仅 `1/64`。加入 24% alpha 对比后，这等价于把
`1/4` effect texel 放大为接近硬方块。修正后 RGB 散射与 alpha 透射率都只使用空间权重和
全分辨率 scene-depth guide，不再由低分辨率解自己的颜色/透射率差拒绝邻居。平坦或连续
表面上的阴影柱因此获得双线性 coverage AA，四个邻居都在暗柱内部时仍精确保留原 24%
上限；只有 scene-depth 真实断层继续阻止跨表面混合。这是 Kopf 联合双边公式中“低分辨率
解 S”与“高分辨率引导图 I”职责分离到 WarVK RGBA effect 的直接映射。

第二轮动态回归说明只修 composite 仍不足：Froxel integrator 原先仅在相机 ray 的参考深度
接近中心 shadow texel 存储深度时启用四点比较。Caster 轮廓横向跨越 shadow-map texel 并不
满足这个 receiver-depth 条件，绝大多数轮廓仍是单 texel 二值判断；快速移动时就整格跳变。
候选改为每个已证明 CSM 覆盖的解析区间都执行一次 `textureGather`，对四个存储深度分别求
Beer-Lambert 加权的 `weightedLitIntegral`，再按 shadow 坐标的亚 texel fraction 双线性合并。
这既不是线性过滤原始深度，也没有把四次独立 texture fetch 乘进整 ray DDA；comparison PCF
使静态与动态 silhouette 在一个 shadow texel footprint 内连续变化。`pcfRadius=0` 仍退化到
nearest comparison，`>=1` 使用完整双线性覆盖；本轮不宣称完成时域超采样，若玩家回归仍有
亚像素爬动，应增加有反应性拒绝的太阳阴影历史，而不能恢复二值边或无条件积累动态拖影。

当前实现是 Chen 方案之前的有界基线，并没有声称完成 epipolar rectification 或
1D min-max mipmap：

- 整条输出 ray 共用 DDA 上限，Medium 为 1024、High 为 2048；预算不会在每个 Z slice
  重新开始。此前 128/256 在低视角抵达 caster 前已经耗尽，随后每层一个中点又形成
  重叠轮廓；
- 每个 DDA 区间执行一次 2×2 depth gather，对四个原始深度分别解析比较后按亚 texel
  覆盖率合并；没有线性过滤深度或四次独立纹理指令；
- 预算耗尽后，每个剩余 Froxel 段使用 4–64 个、最长约 32 世界单位的有界子段，
  不再恢复“每个 Z slice 一个 caster 轮廓”；
- 主 DDA admission budget 为 350,000,000 pixel-steps；耗尽回退另有每 slice 64
  次硬上限，需以前台 GPU 时间门验证，不能把 admission 数字当完整 shader 指令数；
- volume-sun 优先选择能完整覆盖当前段的 near ortho 层，失败后才选 far 层，避免
  始终使用远层丢掉巨型 caster 附近精度；相机 CSM 会向更远层查找完整段覆盖；
- 将来若实机性能或远端 alias 门失败，应增加 min-max/epipolar 加速，而不是恢复
  surface-tail 网格、增加 Froxel Z 层或重新把太阳阴影写入 3D 辐亮度纹理。

## 时间与采样

Inject 对介质和点光采用 2/4 个分层子格样本，并由 `frameSerial & 7` 旋转八相位。
由于 Z 语义重新统一，Temporal 可用 world-position reprojection；历史仍要求相同
map/device epoch、连续帧、相同 near/far，且相机位移不超过 `min(512,f*0.1)`。
七邻域方差裁剪和 reactive rejection 保留。太阳高频阴影不进入历史 Froxel 场，避免
把旧 caster 轮廓长期拖在 3D 历史里。

## 必须完成的物理门

1. 固定 caster 与太阳，连续从俯视压到最低角：caster 到实体影子的暗柱起点不得向
   影子方向滑动，顶部不得按模型是否完整入镜开关。
2. 低视角录制逐帧检查：不得再看到 64/128 份离散 caster 轮廓；允许保留由 shadow
   texel/quarter-res composite 造成的细边 alias，需单独定量。
3. 将全局介质关闭、只保留局部 Sphere/Box/Cylinder：共同 Z 与 surface terminator
   不得把局部 volume 变回全局雾。
4. `d3d9.log` 必须出现 `commonNear=20 commonFar=10000`、独立的 `grid=` 与
   `effect=`、正确 backend、`volumeSun=1` 和 High `shadowSteps=2048`；
   若 volume-sun 无效并回退相机 CSM，不能把回退结果当正式验收。
5. 玩家前台 1080p/4K 分别记录 GPU 时间、device lost、budget fallback；自动化隔离桌面
   不能代替此前台性能门。
6. 对照关闭体积光前后的实体 CSM，表面阴影细节不得改变。
