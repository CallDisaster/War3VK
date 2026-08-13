# WarVK Froxel：远表面尾段、覆盖采样与历史有效性

日期：2026-08-13

> **已否决 / 仅供追溯。** 玩家物理回归证明逐屏幕列 `[L-D,L]` 会随
> 俯仰角滑动，并让同一 Froxel Z 在不同 XY 列表示不同世界深度；这正是
> 阴影从 caster 向地面收缩和轮廓重叠的根因。当前替代合同见
> `docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`。

## 问题与源码证据

玩家低视角观察巨型 caster 时，屏幕上方约五分之一的体积阴影会在一条近似水平的边界外消失；抬高俯角后又逐渐出现。画面中的实体阴影仍存在，因此不能先把它归因于基尔加丹的 Geoset 被屏幕裁剪。

旧 RayMarch 对非天空且表面距离 `L > D` 使用地表端区间 `[L-D,L]`。首版 Froxel 却固定建立 `[near,D]` 的相机端网格，并在积分阶段执行 `min(L,D)`。因此当低视角远地表 `L>D` 时，靠近地表的整段介质被排除，画面表现正是一层随摄像机俯仰移动的距离壳。首版还有两个放大因素：

- 64 个指数 Z slice 在 `D=1400` 的末端厚度约 90 世界单位，二值阴影容易呈现层叠轮廓；
- 每个 Froxel 每帧只有一个抖动采样点，八帧相位加 0.88 历史权重会让细 caster 的阴影延迟出现。

## 一手依据

1. Bart Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*, SIGGRAPH 2014：采用与视锥对齐的 3D 纹理、深度分布和逐 cell 光照；同时明确指出低分辨率会产生欠采样/走样，高频阴影应先变成可滤波的低频可见度，时间抖动需要在噪声、拖影和重投影失效之间取舍。
   https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf
2. W. Reeves, D. Salesin, R. Cook, *Rendering Antialiased Shadows with Depth Maps*, SIGGRAPH 1987：Percentage-Closer Filtering 对多个“深度比较结果”求平均，而不是先滤波原始深度再做一次比较。
   https://dl.acm.org/doi/10.1145/37402.37435
3. Khronos Vulkan Guide, *Synchronization Examples*：图像从深度附件写入转为 shader 读取，以及 compute shader 的读写依赖，必须把实际消费者 stage/access 纳入 barrier；只声明 fragment consumer 不能覆盖 compute 采样。
   https://docs.vulkan.org/guide/latest/synchronization_examples.html

## WarVK 映射

### 1. 每个屏幕列使用表面端积分域

令：

- `n`：Froxel near；
- `D`：作者设置决定的有界积分预算；
- `L`：该屏幕列由全分辨率硬件深度重建的表面距离。

区间定义为：

```text
天空                   [n, D]
非天空，n < L <= D+n   [n, L]
非天空，L > D+n        [L-D, L]
```

远表面使用线性 Z：

```text
z(i) = (L-D) + D * i/N
```

近景/天空仍使用指数 Z：

```text
z(i) = n * exp(log(end/n) * i/N)
```

这保留近相机精度，同时消除固定 `D` 距离壳。例：`L=3000, D=1400, N=64` 时，域为 `[1600,3000]`，每层 21.875 世界单位；它不再是首版末端约 90 单位的厚层。

Inject 与 Integrate 必须使用完全相同的区间公式。Inject 新增全分辨率深度 binding 和 viewport/depth 参数；Integrate 删除 `min(L,D)`。

### 2. 当前帧内的有界覆盖采样

每个 Froxel 使用固定分层子格而非八帧单点抖动：

- Medium：2 个对角样本；
- High：4 个 XY/Z 分层样本。

每个样本独立求介质、太阳/点光和遮挡，然后平均 RGBA。太阳遮挡使用 4-tap PCF，先逐 tap 比较再平均，并消费已有 `pcfRadius`。

最坏边界保持显式：4K High 最多 2,073,600 个 cell、4 个覆盖样本；双层体积太阳最多约 66.4M 次 D32 比较读取。4K Medium 约 391,680 个 cell、2 个覆盖样本、双层太阳约 6.27M 次比较读取。High 是显式高成本质量档，格式/尺寸/2.1M cell admission 失败仍回退 Legacy。

### 3. 历史只允许同一投影域

表面端 Z 是每个屏幕列相对于当前 `L` 的归一化坐标。若不保存上一帧每列的区间，仅用上一帧 view-projection 把世界点映回固定指数 Z 是错误的。

本候选采用保守合同：

- 确定性多点覆盖不再依赖八帧抖动；
- 只有 view-projection 与 camera position 完全相同时才读取同 cell 历史；
- 任意相机移动/俯仰立即使用当前帧；
- radiance/extinction disagreement 可把历史权重降到 0，不再强留 5% 旧结果。

后续若要在运动相机下恢复正确重投影，必须额外保存并验证上一帧每列 `[start,end]`，或改用世界空间/级联体积；不能重新套用首版固定 Z 公式。

### 4. Vulkan 同步

主 CSM、体积太阳复用 CSM、点阴影 cube、体积 pass 的 depth copy 都会被 compute inject/integrate 读取。其读写 layout transition 的 shader stage 现同时包含 `FRAGMENT_SHADER` 与 `COMPUTE_SHADER`，而不是让新 compute consumer 依赖未声明的隐式顺序。

## 非结论与实机门

- 此修复不改变 caster capture/replay，也不宣称巨型多 Geoset 的屏外身份问题已经不存在；若修复后实体阴影和体积阴影仍同步缺同一子部件，应再抓 volume-sun depth/replay manifest。
- 离线编译与静态合同不能替代玩家前台 A/B。必须在相同机位验证低俯角、连续缓慢俯仰、静止 8 秒和 High/Medium 性能。
