# 2026-08-13 Froxel 阴影柱边缘抗锯齿候选

## 玩家回归与根因

玩家确认 true-CSM 可读性候选显著恢复阴影柱，但截图显示柱边出现夸张的大块阶梯。
`war3_volumetric_composite.frag` 此前把低分辨率 effect 自身的 RGB/alpha 强边继续当作
range guide，跨边权重最低只有 `1/64`。在 High 的 `1/4` effect 上，这会把加入的 24%
透射率差作为接近硬方块放大。

## 修改

- RGBA effect 仍以 2x2 空间核重建，并由全分辨率 scene depth 执行联合双边保护。
- 低分辨率 RGB 散射和 alpha 透射率不再拿自身差异作为 range guide；同一连续表面上的
  体积阴影边界获得双线性 coverage AA。
- scene-depth 真实断层仍拒绝跨表面混合；阴影柱内部四个邻居相同时精确保留原强度。
- 不修改表面 CSM、shadow map、Froxel 积分、24% 上限、全局介质或 caster 路径。

依据与 WarVK 映射见
`docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`。本候选遵循
Kopf 等的 Joint Bilateral Upsampling：低分辨率解与独立高分辨率 guide 分离；同时符合
Wronski 对低分辨率体积高频阴影需要低通/抗欠采样的结论。

## 验证与边界

- Froxel 定向静态/数值合同：24/24。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- composite GLSL 由实际 build32 链生成 SPIR-V header，32 位 DLL 链接通过；
  `ninja -C build32 -n` no-work。
- DLL：34,278,777 bytes；SHA-256
  `C68F1A30235C6FA4EA3CE42A6DF6EF46695413295A42AAF650FDD2D3545CC3D5`。
- 尚未部署或启动游戏。双线性 coverage 会将边缘过渡铺在一个低分辨率 texel footprint
  内；是否达到玩家期待的软硬度仍须同机前台 A/B，尤其检查移动相机时是否爬动或过软。
