# 2026-08-13 Froxel 阴影柱可读性候选

## 玩家回归与根因

玩家确认默认进入 Froxel High 后，低视角阴影柱收缩、侧视间隙和多重 caster 轮廓已不再
复现，但阴影柱本身比旧路径弱很多，几乎不可见。源码对照确认 Froxel integrator 只把
CSM 可见度乘入太阳单次散射，而旧 RayMarch 还会把受真实 CSM 遮挡证据授权、最大 24%
的额外衰减写入 effect alpha。清澈/低密度介质中前者的亮暗散射差值过小，因此几何正确
却缺少可读性。

## 修改

- `intervalVisibility` 分离物理可见度与作者对比度；`contrast` 的幂继续控制太阳散射，
  不得自行制造遮挡证据。
- 只有 `selectSegmentCascade` 成功证明有效 CSM 投影的解析区间，才累计 Beer-Lambert
  散射支撑、物理可见度、遮挡光学厚度和局部峰值。缺失覆盖与 fallback 不累计证据。
- 整 ray 用物理/参考积分比解决长射线遮挡稀释，并以真实遮挡光学厚度约束局部峰值。
- 将旧路径固定视角无关的可读性解析移入 Froxel alpha；太阳能量、证据和
  `contrast > 1` 同时成立才生效，额外 base-transmittance 衰减上限仍为 24%。
- 不增加全局雾密度，不修改表面 CSM、caster 生产、级联范围或资源生命周期。

公式、一手依据、Warcraft III 美术常数披露与物理门见
`docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`。

## 验证与边界

- Froxel 定向静态合同：22/22。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- GLSL 由实际 build32 链生成 SPIR-V header，32 位 DLL 链接通过；
  `ninja -C build32 -n` no-work。
- DLL：34,278,777 bytes；SHA-256
  `348B2AABBA72CC3C5A4B79A5C3FA2844967EE44305DA1AC8AD6720F44ADB47BF`。
- 尚未部署或启动游戏；必须由玩家在相同地图、caster、太阳和最低视角实测阴影柱对比度，
  并确认表面 CSM 与关闭体积光时完全一致。24% 上限是美术候选，不是物理常数。
