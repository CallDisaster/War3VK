# 2026-08-13 统一 Froxel / 太阳阴影区间候选

> 已被 `2026-08-13-froxel-full-view-research-fix-candidate.md` 取代。该后续
> 修复把公共域扩大到 10000、将 effect 分辨率从 grid tile 解耦，并替换了
> 过早耗尽的 128/256 步中点回退。

## 原因

玩家回归否决了逐屏幕列 `[L-D,L]` 的 surface-tail 候选：视角越低，暗柱从 caster
一端向实体影子缩短，同时能看到大量重复的 caster 轮廓。源码复核确认该网格随
surface distance 平移，而且相邻 XY 列的 Z 语义不同，不能继续作为 Froxel 坐标。

公式、一手资料、预算和物理门见
`docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`。

## 改动

- 所有 XY 列恢复共同 `[20,6000]` 指数 Z；scene depth 只终止最终积分。
- 删除 Inject 的 scene-depth 与太阳 shadow 求值；它只生成介质消光和有界点光源项。
- Integrate 新增 volume-sun/CSM shadow 输入，通过 shadow texel DDA 和解析深度交点求
  每段连续 lit fraction，再进行 Beer-Lambert 积分。
- Medium/High 的整条 ray DDA 总预算分别为 128/256；耗尽后每个剩余 Froxel 段只作
  一次有界中点比较，最坏读取数为 176/320，而不是 `Z * shadowResolution`。
- 恢复世界位置历史重投影；2/4 点介质覆盖由真实八帧相位旋转，保留方差裁剪与
  reactive rejection。
- volume-sun far ortho 半径与共同 far 均默认为 6000。
- 旧 JASS quality distance 小于 6000 时不会缩短共同 Froxel 网格；大值最多扩到 10000。
- surface-tail 研究和候选文档已标记为“玩家否决”，避免后续误用。

## 验证边界

- 三个 GLSL compute shader 已由 Vulkan SDK `glslangValidator` 独立编译。
- Froxel 静态合同：17/17。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- Win32 DLL 已链接，`ninja -C build32 -n` 为 no-work，`git diff --check` clean。
- DLL：34,254,099 bytes，SHA-256
  `CA67CF7B190AF6D432D92A32C17DB3D808A64759B7A8D9F7B33D70DCA68E04B6`。
- 未部署、未启动游戏、未进行玩家前台视觉或性能验收。
