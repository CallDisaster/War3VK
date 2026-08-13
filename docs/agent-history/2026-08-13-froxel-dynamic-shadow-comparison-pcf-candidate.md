# 2026-08-13 Froxel 动态阴影 Comparison PCF 候选

## 玩家回归与根因

玩家确认 depth-guided RGBA 上采样后仍有阶梯，动态 Caster 更严重。上轮只修复低分辨率
effect 的放大；integrator 仍仅在相机 ray 的 reference depth 接近中心 shadow texel 深度时
启用四点过滤。Caster silhouette 横向跨 shadow texel 不满足该条件，所以多数区间仍使用
单 texel 二值可见度，移动时整格亮暗翻转。

## 修改

- 每个通过完整 CSM 覆盖证明的 DDA/fallback 区间执行一次 `textureGather`，读取 GLSL
  LINEAR footprint 的四个原始深度。
- 对每个深度分别调用 `weightedLitIntegral`，保持解析 ray/depth 交点及 Beer-Lambert
  光学加权；最后才按 shadow 坐标亚 texel fraction 双线性合并 comparison 结果。
- `pcfRadius=0` 保留 nearest comparison，`>=1` 使用完整一 texel 连续覆盖；默认 volume-sun
  1.35 因而进入连续路径。
- 一次 gather 替代四次独立 `texelFetch`；不提高 2048 DDA 步预算，不引入历史拖影。
- 不修改表面 CSM、shadow-map 生产、24% 可读性上限、全局介质或 caster 几何。

一手依据和公式映射记录在
`docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`：Reeves PCF 要求先比较
后过滤；Khronos GLSL 4.60 §8.9.4 定义 gather footprint 与分量顺序。

## 验证与边界

- Froxel 定向静态/数值合同：26/26。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- GLSL/SPIR-V 与 Win32 DLL 已由实际 build32 链编译；`ninja -C build32 -n` no-work。
- DLL：34,278,777 bytes；SHA-256
  `BC303851AAFEE9289D91777E56C23FA88D5A0B35DB72C3423DCA843A1271083F`。
- 尚未部署或启动游戏。玩家需用静止与高速移动 Caster A/B，确认阶梯、爬动及边缘软硬度；
  若只剩亚像素闪动，后续可研究带当前帧差异拒绝的太阳 effect history，不能无条件积累历史。
