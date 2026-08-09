# Issue #4：方向光 receiver / prepass 数值合同

日期：2026-08-10
范围：仅 Issue #4 的方向光 CSM receiver 稳定性；未改动点阴影、Arena、TAA 重构、剔除或跨地图生命周期。

## 已闭合的源码差异

`war3_shadow_receiver.frag`（DirectInline）和
`war3_shadow_visibility.frag`（Prepass/TAA source）现在在正常渲染路径共用以下合同：

- 终态 PCF 一律 comparison-first；原始 depth 只用于 PCSS blocker search、terrain caster mask 和诊断。
- PCSS blocker search 使用同一份 receiver-plane、边界和“整核回退”规则；prepass 不再悄悄退化为另一种半影模型。
- 级联第二层的 light clip、NDC 和 UV 无效时两条路径都保留 primary visibility，不再混入合成的全亮样本。
- clip、NDC、Jacobian、world/view 输入和关键 UBO 标量的非有限值会 fail-soft 为无额外阴影。
- DirectInline 与 Prepass 的 `main` 均在首个逐像素 return 前无条件计算安全的 world/view/linear-depth
  导数；`computeViewNormal` 只消费该预计算值，避免在 non-uniform control flow 内再求导数。
- cascade count、PCF kernel 与 PCSS search extent 在有限范围证明后才参与整数选择或采样偏移；无效
  `u_params`/`u_params6` 统一保留 visibility=1。

这不证明玩家的树影闪烁已经消失。玩家样本为 4096 DirectInline，TAA history 未参与；本次只消除了
Direct/Prepass 的确定性不一致，并把数值退化路径锁为可测合同。

## 数值验证

新增 `war3_shadow_receiver_numeric_contract_test`，以纯值 oracle 覆盖：

1. compare-first 手写 2x2 fallback 在 2048 个 sub-texel 相位上连续，且不会出现整 texel 跳变；
2. 固定 2048 extent 的 `uv * extent - 0.5 / floor / fract` 与两侧 clamp，避免只以抽象 phase 覆盖；
3. 固定 row-major light matrix 的 receiver-plane 正、负斜率、每 tap 深度参考，以及 NDC→UV 的
   `-0.5 * ndcY` 翻转；退化/NaN Jacobian 会触发整核 centre-reference 回退；
4. cascade smoothstep 起点、终点与两侧连续，以及 clip/NDC/UV 无效时严格保留 primary visibility。

现有 `test_shadow_compare_pcf_static.py` 将数值 oracle 与两条 GLSL 实现的 comparison/raw sampler、PCSS、
固定 Y 翻转、UBO 保护以及导数位置绑定；它不替代数值测试，也不构成玩家视觉验收。

## 验证结果

- `test_shadow_compare_pcf_static.py`、`test_shadow_non_taa_continuity_static.py`、
  `test_shadow_taa_v2_static.py` 与 `test_shadow_release_hardening_static.py` 全部通过（36 项）。
- `war3_shadow_receiver_numeric_contract` 通过；Win32 `d3d9.dll` 以 `-j2` 增量构建通过，随后
  `ninja -C build32 -n` 为 no-work。
- 本文记录的是离线数值与结构合同，不包含任何玩家视觉结论。
- 产物不部署、不启动 Warcraft，仍等待固定相机与移动相机物理复审。
