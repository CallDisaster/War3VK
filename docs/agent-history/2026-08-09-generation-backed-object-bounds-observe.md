# Generation-backed object bounds Observe

## 目的

`14a7a66` 已禁止 guessed scene-node sphere 直接裁掉非地形 caster，修复了
建筑、树木和动画附件可能被远级联误杀的问题。本阶段补齐一条可验证、但默认不改变
画面的静态 bounds 证据链，为 Issue #5 的 C2/C3 提前剔除准备 Observe 数据。

## 实现

- `ShadowModelResourceCache` 在签发 `immutableModelGeneration` 后，从该代完整
  position 字节严格派生有限 AABB 与包围球；未解析、捕获失败、非有限数据和长度不精确
  的 payload 清空 bounds。
- `mapEpoch + immutableModelGeneration + localBounds` 经 runtime resource、packet、
  canonical mesh 和 native validation 全链路携带；下游不能自行签发 generation。
- 最终 caster 新增独立 `boundsIdentityProven`。最终 CSM 不再用“generation 非零”冒充
  对象身份证明。
- 只有以下条件同时成立才发布 `ExactLocalGeoset` proof：
  - Building 或 Destructible；
  - rigid、非 frame-local dynamic；
  - current map epoch 与非零 immutable generation；
  - `renderablePart`、`meshData` 与 `runtimeGeosetData` 精确对应；
  - 使用当前 scene-node world matrix；
  - 派生 bounds 有效。
- skinned/动画附件（包括 UBirth）继续 fail-visible。draw-time VB override 会显式清除
  static bounds authority，不能继承旧 proof。
- `DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME` 默认关闭。默认候选只记录 accepted proof、
  fail-visible 和 would-cull；只有显式实验才会在 C2/C3 消费结果。
- final-caster trace 记录 provenance、source generation、frame serial、identity proof
  及 dynamic/skinned/attachment 标志，便于逐 part 对照。

## 验证

- 70/70 `test_*_static.py` 通过。
- 18/18 Win32 Meson runnable 通过；新增 geoset bounds 纯值测试覆盖有限范围、严格
  payload 长度和 NaN/Inf fail-closed。
- Win32 DLL `-j2` 构建通过，`ninja -C build32 -n` 为 no-work，
  `git diff --check` 通过。
- 构建 DLL SHA-256：
  `91BA4B8F3CD3D3E67F95D8385ACC84C74B554B64181F97B0342E8154A1C147C2`。

## 尚未完成

- 未部署、未启动游戏、未进行玩家前台物理画面验收。
- 需要先以默认 Observe 收集至少 10,000 帧，确认 proof cohort 机会率、would-cull
  数量和 false-negative 为零。
- 显式 Consume 必须再做 UBirth、树木、建筑、高压低视角及 A-B-B-A；本提交不能描述为
  Issue #5 已解决。
- 此路径位于最终 CSM replay，尚未减少更早的蒙皮、freeze、Arena reservation 或 packet
  build。真正的提前剔除仍需把当前 CSM consumer volume 安全发布到 producer 前端。
