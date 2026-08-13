# 2026-08-13 体积阴影 Bias 与回退相位修复候选

## 玩家回归与结论

玩家对已部署的全视域 Froxel 候选回归后，确认低俯角及从阴影柱侧面观察时仍有明显亮缝、
分层轮廓，并且阴影柱会向实体投影方向收缩。这次回归否决了“扩大共同 Z 域和增加 DDA
预算即可完整解决问题”的判断。

源码复核分离出两个可在体积路径内独立修复的确定问题：

1. `volumeSunReceiverBias=0.0075` 被直接当作光空间归一化深度使用。当前 far ortho 的
   深度跨度约 20,000 世界单位，因此它会把体积阴影边界推开约 150 世界单位；near 层也约
   70 世界单位。低俯角会将这段 Peter Panning 放大为 Caster 与阴影柱之间的长亮缝。
2. DDA 预算耗尽后的 fallback 在每个 Froxel Z slice 内重新分段并重置采样相位，仍会把
   Caster 轮廓按 64/128 个深度层重复投影，形成侧视可见的层叠结构。

同时确认另有一个上游问题不属于本候选：volume-sun 与表面阴影共用本帧
`BuildShadowReplayDraws` 结果；默认 DirectGrouped/CurrentDraw 来源仍受 Warcraft 本身的
视锥提交影响。模型或部分 geoset 离开游戏提交范围后，短暂 grace 耗尽便可能从体积阴影
生产集合消失。这个问题可以解释玩家所述“压低角度后过一阵才缺少上段”的行为，但修复需要
建立有生命周期、姿态和资源身份保证的离屏 Caster 几何来源，不能用延长陈旧 draw lease
冒充。本候选没有修改这一共享阴影生产合同，也没有与并行优化线程争用该区域。

## 修改

- `volumeSunReceiverBias` 的外部/设置语义改为世界单位，默认 2；发布 volume-sun snapshot
  前按 far ortho 的实际 `maxZ-minZ` 转为归一化深度并限制范围。
- 该转换只作用于独立 volume-sun snapshot，不写入或改变表面 CSM 的 bias、矩阵或资源。
- fallback 的 32 世界单位采样间距锚定到共同 `froxelNear`，每个 slice 继续同一条相机射线
  上的全局格点，不再局部重新分相。
- 静态合同覆盖世界尺度转换、禁止旧固定 `0.0075` 及全局 fallback lattice。
- 公式和一手依据见
  `docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`。

## 验证与边界

- Froxel 静态合同：20/20。
- 局部体积雾静态合同：10/10。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Win32 DLL 构建通过；`ninja -C build32 -n` no-work。
- DLL：34,270,447 bytes，SHA-256
  `E14F7C60F904E6A89FFA03E2145F956C5E2E45E1A9AFE7463B28C0F4F7042D19`。
- 产物位于 `build32/src/d3d9/d3d9.dll`。
- 本候选未部署、未启动游戏，也未完成玩家前台低俯角/侧视物理 A/B。
- 本候选只声称修复上述两个确定的体积路径问题；不声称已解决离屏 Caster 几何来源。
