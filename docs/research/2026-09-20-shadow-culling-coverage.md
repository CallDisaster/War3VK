# 阴影剔除真实覆盖与可证明的早期减量边界

- taskId: `culling-b01`
- sessionId: `session-e24b90ff-4d98-4d31-bbac-10cf1db2a9de`
- 父任务: `01a02e0b-1d1e-7762-b40b-63a00bbb3449`
- 日期: 2026-09-20（台北）
- 状态: 只读研究；新增 Python 静态测试、C++ 反例测试；无生产代码修改、无编译、无部署、无实机。

## 0. 结论

1. **最终 draw 变少不等于 384 MiB 快照池收缩。** Stage11 snapshot 在
   `War3TryCaptureShadowCaster` 内分配（`d3d9_device.cpp:43323-43334`），
   远早于 `renderShadowMap` 的最终级联掩码（`d3d9_war3_shadow.cpp:4290-4536`）
   和 record 循环（`4809-4830`）。final cull 只改 draw 列表和 workload，不回收
   `m_war3Stage11SnapshotPages`；页只在 `use_count()==1` 时由
   `War3CollectUnusedStage11SnapshotPages`（`d3d9_device.cpp:23826-23844`）回收。
2. **今天没有默认真实 Consume。**
   - 地形 runtime 只返回 Off/Observe；实际屏蔽还要求
     `terrainBoundsCullMode==Consume && c>=2`。
   - 对象 Consume 被 `!kReleaseFreezeExperimentalShadowRoutes` 切断；本树
     freeze=true（`war3_internal_test_config.h:23`）。
   - union 调用点显式 `query.consumeAdmissionGranted=false`，effective mask 不变。
3. **低视角难剔不是公式算不动，而是合法证据链太窄。** 只允许 C2/C3、
   exact static/current bounds；动态/蒙皮/未知/动画附件全部 fail-visible。
   保守半径与保护带放大 NDC 球；低 RTS 视角让 CSM 稳定球覆盖更大世界范围。
4. **不能直接打开现有 capture coarse cull。**
   `kShadowCaptureCoarseCullEnabled=false`（`war3_internal_test_config.h:1081`）；
   实现 `d3d9_device.cpp:49234-49288` 用玩家相机 `worldCamera.viewProj` 和通用
   估算半径裁 decoration-like draw，不满足完整 caster light volume 之外的合同，
   也没有 exact provenance 门。
5. **当前公式只对 orthographic CSM 已证明。** 生产 CSM 用
   `makeOrthoOffCenterLH` 且 w 行为 (0,0,0,1)
   （`d3d9_war3_csm.cpp:107-120,610-614`）。union policy 不检查 w 行，也没有
   map/device epoch；下一批要把合同显式化。

## 1. 生命周期与证据位置

| 阶段 | 位置 | 结论 |
| --- | --- | --- |
| Draw-time capture | `d3d9_device.cpp:40652` | 冻结/缓存 VB、IB、UV、blend；snapshot 分配入口 |
| Position snapshot | `d3d9_device.cpp:43323-43334` | `War3AllocateStage11Snapshot` 在 384 MiB resident cap 内分配/复用页 |
| UV/index snapshot | `d3d9_device.cpp:43591-43603,43747-43759` | 同页继续 suballocate |
| Legacy terrain bounds | `d3d9_device.cpp:49039-49223` | exact span 扫描；但 publish 在 `50148-50169`，晚于 snapshot |
| Replay resolve/prepare | `d3d9_war3_shadow.cpp:3828-3841,4132-4265` | snapshot 已存在；prepare 不做级联剔除 |
| Final cull / union observe | `d3d9_war3_shadow.cpp:4290-4536,4545-4675` | 只影响后续 draw 列表；union 不消费 |
| Per-cascade record | `d3d9_war3_shadow.cpp:4809-4830` | 从这里才开始真正的最终提交减量 |

m_csmData = newCsm 在 receiver pass 的 renderShadowMap 前发生；
draw capture 时只有上一完整帧的 m_csmData。
因此 capture 前用完整当前帧 light volume 剔除不是现成小改。

## 2. 主路径覆盖与消费现状

- S1 terrain exact span：exact position span -> local bounds -> world，ExactCurrentWorld；今天不消费。
- S1 terrain early-cache：immutable local bounds + 当前 world matrix，ExactLocalGeoset；今天不消费。
- S1 terrain compat persistent：geometry local bounds + compat world matrix，ExactLocalGeoset；今天不消费。
- Static building/destructible semantic submit：canonical immutable local geoset + SceneNode matrix，ExactLocalGeoset；对象 Consume 被 freeze 切断。
- Unit/Effect/skinned：通用诊断半径或 semantic 递推，Unknown/GenericDiagnostic；fail-visible。
- Animated attachment：draw-time override 清空 authority，Unknown/dynamic/animated；fail-visible。
- Legacy non-terrain fallback：未 publish exact bounds，默认 Unknown/radius=0；fail-visible。
- Union observer：复用上述 authority，要求 stage11 RequiredCurrent/camera current/resource gen；只预测。

模式：terrain runtime 永远 Off/Observe，不返回 Consume；
object consume 被 release freeze=true 切断；union runtime 永远 Off/Observe，
调用点 admission=false。capture coarse cull constexpr=false，人工打开会跳过 capture。

## 3. 为什么 final draw 少不等于快照少

- snapshot 在 capture 分配，final cull 在 prepare 后；时序不同。
- 页只在 use_count==1 时回收，final 不可见不会释放页。
- 页内 used 单调增长，已分配 suballocation 不会因 final 剔除形成可复用空洞。
- 384 MiB 是 resident cap；final cull 减的是级联 draw 和工作量预约，不是 page 数。
- capture 时当前帧完整 CSM light volume 尚不可用，所以早期剔除不是现成小改。

## 4. 可测上界

- final 级联 draw：最多少 2*q*E 个 cascade item，占原始 sum(V_c) 的比例 2qE/sum(V_c)；不减少 prepare pipeline 数。
- snapshot 字节估算：每个 slice 独立 suballocation，故 B_i_est = sum_s align256(bytes_s)，不能写成单次 align256(pos+blend+uv+index)；各 slice 可能跨 page 或新开 page。
- 实际只认 counter 与页账：drawTimeSnapshotSuballocationBytes/Resident used/page create/reclaim 是 ground truth，估算只作规划。
- 早期减量只有在 capture 前拿到当前帧完整 light volume 且全消费者闭合 outside 时，才可能少请求 sum(B_i_est)。
- 历史 BF938：resident=402,653,184 B（384 MiB），一个窗口 capacityReject +37,446，另一窗口 suballocationBytes=863,311,872 / 3,286 次，平均约 262,724 B/次；仅作量级参考。

## 5. 空间/公式合同

当前 CSM 和 volume-sun 都是 orthographic，w 行恒为 (0,0,0,1)，因此 row-length 球半径上界保守；负 scale 由平方和处理。
未进入 policy 的合同：union policy 不验证 row3==(0,0,0,1)，perspective 可能 false negative；
War3ShadowBoundsCullEvidence 没有 mapEpoch/deviceEpoch；
evidence 不携带 coordinate space / parent transform / world producer 合同；
ExactCurrentWorld + frameLocalDynamic=true 是 S1 当前帧特例，调用方必须继续保证 boundsFrameSerial=current+1 同源。

## 6. 测试与下一批

C++ 新文件 AutoTest/test_shadow_culling_counterexamples.cpp 直接调用共享 policy，覆盖旋转/缩放/负 scale/边界接触/NaN/陈旧代际/动态/未知/Consume admission/off-volume；编译时链接 src/d3d9/war3/render/war3_union_consumer_visibility.cpp。
Python 新文件 AutoTest/test_shadow_culling_coverage_static.py 已在准许范围内运行通过。
推荐下一批 A（先做）：Observe-only 数据闭环。在已有 m_csmData 的同一帧记录 exact bounds 通过情况、C2/C3/全级联 outside、boundsGeometryBytes、wouldSkipIfEarly；capture 侧记录 snapshot 请求 bytes 和拒绝原因，不改 authority。
推荐下一批 B：final Consume，默认关闭，先 Observe 后 A/B。扩展 evidence 增加 map/device epoch 和 orthographic 合同；只允许 C2/C3、exact static/current、same frame/map/device/resource、finite positive radius；保持独立 admission。
推荐下一批 C（较大）：早期 snapshot 减量。需要把当前帧 CSM/light volume 提前到 draw capture 前，或做两阶段 capture，仅四层全 outside 的 exact bounds draw 跳过 snapshot 请求；不能把 Observe 默认成 Consume。

## 7. 未覆盖门 / 父任务裁定

未编译 C++，交 validation lane 独占编译；未实机/GPU/视觉；未实际 Consume；release freeze 未解除。
需要裁定：是否加 map/device epoch 与 ortho 合同位；是否允许候选 A 的 Observe-only 计数器；是否投入候选 C 的 CSM 提前计算/两阶段 capture；kShadowCaptureCoarseCullEnabled 是否保持 false 或删除。

## 8. 源码索引

snapshot cap/分配/回收：war3_stage11_snapshot_page_policy.h:17,22,26-27；d3d9_device.cpp:23689,43326,43594,43750,23826-23844。
capture / exact bounds：d3d9_device.cpp:40652,44405-44424,48763-48782,49039-49223,50148-50169,22007-22045。
final cull / union：d3d9_war3_shadow.cpp:4132-4265,4290-4536,4545-4675,4809-4830,11120-11184。
CSM ortho：d3d9_war3_csm.cpp:107-120,610-614。release freeze/cull 参数：war3_internal_test_config.h:23,1018-1083。

## 9. 2026-09-20 culling-b05 dated correction

This section preserves the earlier history above and corrects only conclusions
that were made before b02-b05.

- The b01 statement that union policy does not validate the orthographic w row
  or map/device epoch is historical. b02/b03 added exact w-row validation
  (`(0,0,0,1)`) and map/device unknown/mismatch reasons. b03/b04 mapped
  `input.mapEpoch/deviceEpoch`, `draw.mapEpoch/deviceEpoch`, and
  `m_shadowMapEpoch/m_shadowDeviceEpoch` into the observer query.
- This is still not all-consumer proof. Production requests only CSM2 and CSM3
  (`d3d9_war3_shadow.cpp:4595-4596`). Main, CSM0, CSM1, volume-sun,
  point-shadow and outline are not closed by that query. Volume-sun has no
  `War3UnionConsumerBits` bit at all. A cleared CSM2/CSM3 bit or a zero
  `predictedVisibleMask` must not be read as permission for early omission.
- Production observer source gaps remain: candidate/camera/consumer frame
  generations copy `input.frameSerial`; `War3WorldCameraIsFreshForFrame`
  permits a one-frame-old camera fallback; `resourceGeneration` and
  `expectedResourceGeneration` are both `m_shadowMapResourceGeneration`; and
  there is no matrix-generation field binding `m_csmData` to the current
  candidate. These are recorded for a future minimal adapter, not fixed here.
- The C++ counterexamples added by b05 are pure-policy synthetic inputs. They
  test requested masks, stale generations, missing matrix, resource mismatch
  and absent closure. They are not pass integration and do not prove GPU
  geometry, image correctness, or snapshot-byte reduction.
- `unionCullFalseNegativeCount` compares against the canonical CPU replay mask,
  not GPU/image ground truth. No no-shadow-leak or snapshot-bytes-saved claim
  is made by this correction.

