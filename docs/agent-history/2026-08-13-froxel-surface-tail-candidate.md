# 2026-08-13 Froxel 远表面尾段候选

> **候选已被玩家物理回归否决，不得部署或作为当前实现说明。** `[L-D,L]`
> 只是把裁剪面从远端移到了近端，同时破坏全屏统一的 Froxel Z 语义。替代候选见
> `docs/agent-history/2026-08-13-common-froxel-shadow-interval-candidate.md`。

## 结论

研究端对“固定 `[20,1400]` 距离壳、单点阴影覆盖和 8 帧历史延迟”的判断与当前源码一致。本候选不扩大距离上限，而是把有实体深度的远屏幕列恢复为表面端 `[L-D,L]`，并改用线性远端 slice、当前帧分层覆盖和保守历史有效性。

## 改动

- Froxel Inject 新增 scene depth binding，和 Integrate 共用 surface-ended ray interval。
- `L>D` 的远表面采用线性 Z；近景与天空保留指数 Z。
- Medium/High 分别使用 2/4 个确定性覆盖样本；CSM 改为消费 `pcfRadius` 的 4-tap comparison average。
- 删除八帧抖动相位；相机不完全稳定时禁用历史，当前变化可完全替换旧结果。
- CSM/volume-sun、point-shadow cube 与 depth-copy barrier 纳入 compute shader reader。
- 新增公式、预算、同步与后续实机门研究记录：`docs/research/2026-08-13-warvk-froxel-surface-tail-and-coverage.md`。

## 验证边界

- 三个 Froxel GLSL 由 Vulkan SDK `glslangValidator -V` 独立编译通过。
- `AutoTest/test_war3_volumetric_froxel_static.py`：15/15。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- Win32 `build32/src/d3d9/d3d9.dll` 已链接；`ninja -C build32 -n` no-work；`git diff --check` clean。
- DLL：34,245,871 bytes，SHA-256
  `4C9B19B8CC2F3F009001A4643B89446E63B42423F19CA1DBF3CDCAAD1FA1BD24`。
- 未部署、未启动游戏、未完成玩家前台物理/性能 A/B。
