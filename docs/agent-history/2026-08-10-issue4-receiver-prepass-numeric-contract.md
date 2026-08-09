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

这不证明玩家的树影闪烁已经消失。玩家样本为 4096 DirectInline，TAA history 未参与；本次只消除了
Direct/Prepass 的确定性不一致，并把数值退化路径锁为可测合同。

## 数值验证

新增 `war3_shadow_receiver_numeric_contract_test`，以纯值 oracle 覆盖：

1. compare-first 手写 2x2 fallback 在 2048 个 sub-texel 相位上连续，且不会出现整 texel 跳变；
2. 正、负斜率 receiver-plane 的 UV 符号、每 tap 深度参考和退化/NaN Jacobian 的整核 centre-reference 回退；
3. cascade smoothstep 两侧连续，以及 next cascade 无效时严格保留 primary visibility。

现有 `test_shadow_compare_pcf_static.py` 只用于把上述数值 oracle 与两条 GLSL 实现的 comparison/raw sampler、PCSS 和有限性保护绑定；它不替代数值测试。

## 验证结果

- 7 个相关 Python static 套件通过；
- `war3_shadow_receiver_numeric_contract` 通过；
- 28/28 Win32 Meson runnable 通过；
- Win32 `d3d9.dll` 以 `-j2` 构建通过；`ninja -C build32 -n` 为 no-work；
- 产物未部署、未启动 Warcraft，等待固定相机与移动相机物理复审。
