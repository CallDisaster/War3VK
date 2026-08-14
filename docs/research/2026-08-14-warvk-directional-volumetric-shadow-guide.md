# WarVK 独立方向体积阴影 Guide 与边缘细化合同

日期：2026-08-14

## 问题

当前 Froxel 的介质/点光历史是三维时域场，但太阳可见度直到二维 Integrate 才计算。
High 的二维结果通常只有全分辨率的 `1/4`，预算不足时为 `1/8`。场景深度只能保护
实体几何断层，不能区分同一地面上的亮空气与体积阴影；在低分辨率阶段执行小于 1
的可读性指数还会把少量 Caster 覆盖扩大到整个 effect texel。

## 一手依据

1. Bart Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to
   Atmospheric Scattering*, SIGGRAPH 2014。该方案明确记录低分辨率体积结果在边缘处的
   欠采样，并使用邻域/双边重建与历史采样处理空间和时间稳定性：
   <https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf>
2. Johannes Kopf、Michael F. Cohen、Dani Lischinski、Matt Uyttendaele，
   *Joint Bilateral Upsampling*, ACM TOG 2007。低分辨率解由空间核重建，range 权重必须
   来自独立的高分辨率 guide；不能让低分辨率解自己锁定其块状边界：
   <https://johanneskopf.de/publications/jbu/paper/FinalPaper_0185.pdf>
3. Reeves、Salesin、Cook，*Rendering Antialiased Shadows with Depth Maps*,
   SIGGRAPH 1987。PCF 必须先完成每个深度比较，再过滤比较结果：
   <https://dl.acm.org/doi/10.1145/37402.37435>
4. Microsoft, *Cascaded Shadow Maps*。过大的 light-space texel 对 camera pixel
   会产生块状透视走样；近处需要更高 shadow-map 密度，级联边界应有受控重叠：
   <https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps>
5. Khronos, *GLSL 4.60 Specification*, texture gather。`textureGather` 的线性
   footprint 由 texel center/half-texel 边界决定；解析 DDA 必须在 footprint 改变处
   重新取样：
   <https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#texture-gather-functions>
6. Khronos, *Vulkan Synchronization Examples*。compute storage write 到后续
   compute sampled read、再到 fragment sampled read 必须建立显式可见性与布局转换：
   <https://docs.vulkan.org/guide/latest/synchronization_examples.html>

## WarVK 映射

本候选不改变 Caster、CSM、Volume Sun、Froxel 介质或点光身份。它在现有
`War3VolumetricLightPass` 内增加两个 `R16_SFLOAT` 方向阴影 guide：

- base guide 与现有 effect 同分辨率，Integrate 写入由真实 CSM 区间支持的物理遮挡；
- refined guide 默认是全分辨率的 `1/2`。其 shader 先检查 base guide 的 3x3
  min/max；平滑区域只做重建，只有边缘像素才重新执行方向阴影区间积分。

边缘细化是 WarVK 的有界工程调度，不是上述论文中的固定常数。它有独立的
pixel/step admission：若目标尺寸或格式不满足合同，则保留 base guide；若单 ray 的
细化 DDA 耗尽，则该像素回退 base guide，绝不发布不完整积分。

现有二维 effect 只保存散射和物理消光，不再在低分辨率阶段压暗底图。Composite 先用：

```text
G = {full-resolution receiver plane, refined directional shadow guide}
```

重建 effect，再在输出像素上应用有界可读性曲线。接收面由 full-resolution depth 反投影
的中心、左右和上下邻域估计；range distance 使用样本到中心切平面的距离，而不是非线性的
raw depth 差。遮挡接近零时曲线保持线性，不使用指数小于 1 的放大。

玩家前台回归随后暴露了两个与重建顺序独立的可读性回归。第一，Composite 的旧空介质
提前返回位于可读性项之前，导致散射与物理消光接近零时，即使独立 guide 已证明存在 CSM
遮挡也直接输出原图。第二，只使用整条视线的平均遮挡会让“小 Caster + 长射线”的信号趋近
零。修订合同因此要求：空介质早退必须同时证明 guide 可读性项为零；guide 可继续使用旧路径
已有的、受 `shadowEvidenceOptical` 双重约束且上限为 `0.74/0.68` 的峰值遮挡证据。峰值项不再
直接写入 `1/4` RGBA effect，而是先经过独立 guide 的边缘细化与最终重建，仍不恢复低分辨率
自引导或小于 1 的遮挡指数。默认档最终底图衰减仍受 24% 硬上限约束。

## PCF/DDA 一致性

若 shadow filter coordinate 的 texel center 位于 `n+0.5`，双线性 gather footprint
改变的位置也是 `n+0.5`。DDA 因此在 shifted coordinate `coord-0.5` 的整数边界拆段：

```text
boundary = next_integer(coord - 0.5) + 0.5
```

每段仍对四个原始深度分别求 Beer-Lambert comparison integral 后按亚 texel 权重合并，
不线性过滤原始深度。

## 验收边界

静态测试、shader 编译、Win32 runnable、32 位 DLL link 与 Ninja no-work 只能证明 ABI、
资源/同步和有界回退合同。玩家仍须以前台相同地图完成：静止/移动小单位、侧视与最低俯角、
1080p/4K、关闭体积光后的表面 CSM 不变，以及日志中实际 backend/guide 分辨率与回退状态。
