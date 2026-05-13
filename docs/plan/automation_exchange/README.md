# Automation Exchange

Date: 2026-04-18

## Purpose

这个目录是后续自动化提示、长时间接力开发、以及多轮状态交换的固定入口。

使用规则固定为：

1. 先读 `AUTOMATION_MISSION_2026_04_18.md`
2. 再读 `CURRENT_STATUS_2026_04_18.md`
3. 每完成一轮有效推进后，必须回写 `CURRENT_STATUS_2026_04_18.md`
4. 若本轮方法已被后台验证证明无效，也必须把“为什么无效”写回状态页，避免重复试错

## Files

1. `AUTOMATION_MISSION_2026_04_18.md`
   说明项目最终目标、完成标准、禁止回退点、自动化执行约束。
2. `CURRENT_STATUS_2026_04_18.md`
   说明当前真实状态、已经完成到哪一步、当前主阻塞、下一步优先级。
3. `STATUS_UPDATE_TEMPLATE_2026_04_18.md`
   规定每轮更新状态时的固定格式。

## Current Ground Truth

当前最重要的几份前置文档仍然是：

1. `docs/plan/semantic_shadow_control_plane_status_2026_04_17.md`
2. `docs/plan/upper_layer_shadow_cutover_status_2026_04_16.md`
3. `docs/plan/war3_runtime_rtti_ground_truth_2026_04_16.md`
4. `docs/plan/war3_runtime_model_geoset_alias_ground_truth_2026_04_17.md`
5. `docs/plan/war3_unit_shadow_mesh_stream_probe_2026_04_17.md`

## 2026-05-06 Long-Term Plan Entry

当前除了状态页之外，还应优先阅读：

1. `docs/plan/semantic_render_takeover_master_plan_2026_05_06.md`
2. `docs/plan/semantic_render_legacy_path_freeze_2026_05_06.md`
3. `docs/plan/automation_exchange/CURRENT_STATUS_2026_05_06.md`

这三份文档共同定义：

1. 长期路线
2. 旧路径冻结纪律
3. Phase 1 当前实际推进位置

这个目录不替代以上研究资料，而是作为“自动化接力入口”和“当前工作面汇总”。

## 2026-05-08 Latest Continuation

最新状态继续写在 `docs/plan/automation_exchange/CURRENT_STATUS_2026_05_06.md` 的第 16 节：

1. `Phase 5` alpha/cutout canonical producer + consumer gates 已通过隔离桌面验证；
2. `Phase 6` native canonical consume 最小闭环保持通过；
3. `War3SemanticScene/Populate` 的 material diagnostics 重复解码热点已修正，动态高压场景已从不可观察拉回可继续视觉验收范围；
4. 后续默认进入视觉正确性专项或 `Phase 7`，不再把 cutout/alpha producer 或 native consume 当作当前 blocker。

2026-05-08 中午复核后，当前 continuation 已进入第 17 节：

1. `dynamic_shadow_pressure` 隔离桌面复核仍保持 `semanticSceneSubmittedSkinned/Cutout/AlphaBlend > 0`；
2. `objectFallbackDrawCount = 0` 仍成立；
3. `avgFps` 约 `20`，项目可观察但仍需要性能收口；
4. `Phase 7` 第一轮重点固定为：视觉稳定性、receiver/native consume 一致性、`Other/UntrackedActive` 成本拆解，以及 `War3SemanticScene/Populate` 二次优化。

2026-05-08 Phase 7 第一轮视觉止血后，当前 continuation 已进入第 18 节：

1. `cutout / alpha-blend` 数据层可达，但默认不再作为 caster 提交，直到 alpha texture + UV contract 权威化；
2. `DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER=1` 默认拒绝不安全透明 caster，避免单位周围方形阴影卡片；
3. `ShadowPath_StaticStamp_Toggle(0x74E420)` 窄 hook 已启用，mode1 下拦截 enable、放行 disable；
4. semantic dynamic/skinned caster 活跃时禁用 shadow TAA/history；
5. direct current-draw 现在先做轻量 material 预筛，再进入完整 packet/canonical build；
6. 当前默认 cap 固定为 `recordCap=24 / scanCap=96`，这是本轮隔离桌面矩阵里性能和提交密度的折中点；
7. 最新 `dynamic_shadow_pressure`：`avgFps=23.145`，`War3SemanticScene/Populate avgCpuMs=10.864`，`semanticSceneSubmittedSkinned=9106`，`objectFallbackDrawCount=0`，`semanticSceneSubmittedCutout/AlphaBlend=0`。

2026-05-08 Phase 7.2 单caster闪烁根因定位，continuation 已进入第 19 节：

1. 新增 `ComputeStableGroupContentHash()` 不含 stream1Ptr；
2. Phase 7.1 诊断修正：identity hash 基于实际 submitted set，recordCap skip vs append fail 分开统计；
3. object-grouped selection 改用稳定 priorityScore 排序，不再依赖 captureSerial；
4. 新增单 caster contract 稳定性 churn 诊断和 submitted/replay/executed 对账；
5. `ninja -C build32` 通过，待 AutoTest 验证。

2026-05-10 Phase 7.21 slice/manifest 复核后，当前 continuation 进入
`PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md`：

1. 新增 slice-aware visible lookup，direct current-draw shadow 路径优先按
   `renderablePart + layerIndex` 查 visible backing；
2. `payloadWord11C` 暂时从 submitted/sticky part identity 中降级，避免
   cull/variant discriminator 放大 identity churn；
3. slice-aware lookup 已补最小热索引，避免在 visible index deferred 模式下
   每次查询线性扫描整个 snapshot；
4. strict layer gating 与 stable-key publication map 均经 AutoTest 证明不是
   当前止血方案，已回收为后续 manifest 设计证据；
5. 最新 hot poll 仍显示 receiver hold/reuse 为 0，但 replay/submitted set 继续
   抖动，根因仍在 current-draw/manifest eligible part set；
6. 已补最小 Shadow Manifest 诊断和 part/object Jaccard、visible lookup
   exact/weak/miss、multi-slice、payload11C churn counters；
7. `phase721_mainworld_scene_probe` hot poll 显示 submitted skinned 基本全部
   具备 main-world visible backing，且 `MissingUnitPtr / TransparentQueue /
   GroupNonZero` 都为 `0`，因此残余闪烁当前不应优先归因于 preview/portrait
   泄露；
8. 同一轮 hot poll 中 `scanHit=0 / capHit=0 / partialObjects=0`，但
   replay/submitted count 和 part Jaccard 仍抖动，说明问题不是 cap，而是
   current-draw observation miss 被立即当成 caster disappearance；
9. 已落地一帧 `DXVK_WAR3_SEMANTIC_DIRECT_PART_PACKET_LEASE` 局部止血：
   只缓存成功提交且 main-world backing 通过的自持有 skinned packet；
   enabled A/B 将 `identityHashUniqueCount` 从关闭时的 `78` 降到 `49`，
   part Jaccard min 从约 `720` 抬到 `930`，且 receiver hold/reuse 仍为 `0`；
10. 这证明短 TTL 的局部 manifest/part lease 是正确方向，但还不是最终架构。
    下一步应补专用 lease counters，并把 cache 从 direct loop 移到持久
    Shadow Manifest 层，让 current-draw 只更新 pose/slice evidence。

2026-05-10 Phase 7.22 lease cleanup 后，当前 continuation 仍在
`PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 继续追加：

1. 一帧 direct part packet lease cache 已从函数局部 `static` map 收口为
   `D3D9DeviceEx` 私有成员 PIMPL，避免跨设备/静态生命周期问题；
2. lease restore 不再挤占 `semanticSceneDirectStickyFillAppendedCount`，
   sticky-fill 与 part-packet lease 的诊断口径已分开；
3. 新增 `semanticSceneDirectPartLeaseRestored/Updated/Expired/Rejected*/Budget`
   counters，并已接到 runtime bridge、diagnostics hub、control plane、perf
   report 和 `phase720_hot_shadow_poll.py`；
4. `ninja -C build32` 通过；
5. enabled A/B:
   `phase720_phase722_part_lease_counters_v2_hot_shadow_poll_20260509.json`
   显示 `identityHashUniqueCount=61`、`identityChurnSamples=103/134`、
   `partLeaseRestoredMax=11`、`submittedObjectJaccardMin=987`、
   `submittedPartJaccardMin=961`、receiver `reuse/hold=0`；
6. disabled A/B:
   `phase720_phase722_part_lease_counters_disabled_ab_hot_shadow_poll_20260509.json`
   显示 `identityHashUniqueCount=77`、`identityChurnSamples=121/134`、
   `submittedObjectJaccardMin=888`、`submittedPartJaccardMin=868`；
7. 结论：lease 确实在补 current-draw observation miss，不是提高 cap 或
   receiver hold 的假修复。下一步应把该一帧 lease 迁移成 Shadow Manifest
   的结构/姿态/切片/last-good packet TTL 状态，skinned pose TTL 仍保持一帧。

2026-05-10 Phase 7.23 manifest TTL/key A/B 后，当前 continuation 仍在
`PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 继续追加：

1. `phase723_manifest_ttl_diag` 证明 manifest 里仍存在大量短时
   leaseable/stale part，但 receiver `reuse/hold=0`，残余闪烁仍在上游；
2. `DXVK_WAR3_SEMANTIC_DIRECT_PART_PACKET_LEASE_FRAMES=2` 的 A/B 虽让
   `identityHashUniqueCount` 降到 `51`，但 object/part Jaccard 最低掉到
   `747/783`，不能作为默认修复；
3. snapshot dedupe 改成 coarse stable part key 的 A/B 也失败：
   `submittedObjectJaccardMin=796`、`submittedPartJaccardMin=814`、
   `partLeaseRestoredMax=19`，该行为改动已回收；
4. 保留的改动是 manifest 诊断层去掉 `renderablePart` 作为持久 part key，
   并新增
   `semanticSceneShadowManifestRenderablePartChurnCount`；
5. final recheck:
   `phase720_phase723_manifest_key_final_recheck_hot_shadow_poll_20260509.json`
   显示 `identityHashUniqueCount=51`、`submittedObjectJaccardMin=949`、
   `submittedPartJaccardMin=956`、`partLeaseRestoredMax=9`、
   `shadowManifestRenderablePartChurnMax=18`；
6. 下一步应进入 manifest-owned part lease / last-good metadata，而不是继续
   拉长 direct packet lease 或粗改 snapshot dedupe。

2026-05-10 Phase 7.24 manifest-owned lease A/B 后，当前 continuation 继续在
`PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 追加：

1. direct-loop part lease 已改为使用 manifest stable part key：stable object
   identity + `layerIndex + payloadWord108`，禁止 `renderablePart`、
   `meshPayloadPtr`、`stream1Ptr`、`payloadWord11C` 进入 lease/sticky key；
2. `jHandle` 现在优先于 `unitPtr` 作为 object key；A/B 显示关闭 manifest
   lease 会把 `identityHashUniqueCount` 拉到 `95`，object/part Jaccard 掉到
   `794/773`；
3. provisional manifest parts 默认延迟到第二次观测后再提交，避免新对象尖峰把
   steady set 冲散；
4. 3-frame self-contained cached geometry 对 identity churn 有帮助，但 4-frame
   A/B 会恶化 Jaccard，不能作为继续加 TTL 的修复；
5. 现在问题已从 object-level churn 收敛到 part/core completeness：下一步进入
   Phase 7.25，按上一帧完整 core part set 做 object-level core gate。

2026-05-10 Phase 7.25 CModel pose restore probe 后，当前 continuation 继续在
`PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 追加：

1. CModel pose restore 只作为实验开关保留，默认关闭：
   `DXVK_WAR3_SEMANTIC_MANIFEST_CMODEL_POSE_RESTORE=0`；
2. enabled A/B
   `phase720_phase725_cmodel_pose_restore_ab_hot_shadow_poll_20260509.json`
   显示 `identityHashUniqueCount=25`、`submittedObjectJaccardMin=910`、
   `submittedPartJaccardMin=906`、`manifestPartLeasePoseFreshenedFromCModelMax=11`；
3. CModel-backed geometry retention A/B
   `phase720_phase725_cmodel_core_geometry_ab_hot_shadow_poll_20260509.json`
   明确失败：`submittedObjectJaccardMin=754`、`submittedPartJaccardMin=788`，
   该行为改动已撤回；
4. default-off recheck
   `phase720_phase725_cmodel_restore_default_off_recheck_hot_shadow_poll_20260509.json`
   显示 receiver `reuse/hold=0`，但 `manifestPartLeaseExpiredMax=13`、
   `manifestPartLeaseRejectPoseStaleMax=12`，残余闪烁仍在
   current-draw/manifest observation miss；
5. 下一任代理应接着做真正的 object-core epoch planner，而不是继续延长
   geometry TTL、开启 receiver hold、或推进 cutout/TAA。

2026-05-10 Phase 7.25 真正 Object-Core Epoch Planner 落地后，当前 continuation
继续在 `PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 追加：

1. 真正实现了 object-core epoch planner：`D3D9DeviceEx::War3SemanticDirectPartPacketLeaseState::ObjectCoreSet`
   从单一 `partKeys` 拆分为 `committedPartKeys + observationPartKeys`；
   core epoch 只有在 live（非 lease）submitted 集合“包含现有 committed”
   或“双帧 observation 超集一致”时才推进，不再因为一次 current-draw 观察
   丢帧而退化。
2. 新增五个专属计数器，已贯通 scene stats -> runtime bridge -> diagnostics hub
   -> control plane -> perf monitor -> `AutoTest/phase720_hot_shadow_poll.py`：
   `semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount`、
   `semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount`、
   `semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount`、
   `semanticSceneShadowManifestObjectCoreEpochMissingPartCount`、
   `semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount`。
3. 提交阶段 core gate 升级为“live + safe-lease 必须完整覆盖 committed core，
   否则整 object 跳过”，并区分 `UpdatedFromLive`（live 自证完整）与
   `RestoredComplete`（lease 补齐完整）两种 complete 路径。
4. 新增 `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_EPOCH_PLANNER` 开关，默认启用；
   关闭时回到旧“live 全覆盖 commit”语义，便于回归。
5. restored/leased packet 推进 core 在 `packetSafeForDirectPartLease` 前置
   拒绝，并在新计数器 `SelfRenewRejectCount` 留下证据链。
6. AutoTest A/B（`phase720_phase725_core_epoch_planner_final` vs
   `phase720_phase725_core_epoch_disabled_ab`）：
   - Core Epoch 启用：`identityChurnSamples = 30/134`、
     `identityHashUniqueCount = 32`、
     `submittedObjectJaccard Mean/Median = 993.19 / 1000`、
     `submittedPartJaccard Mean/Median = 992.46 / 1000`、
     `manifestCoreEpochUpdatedFromLiveMax = 94`、
     `manifestCoreEpochRestoredCompleteMax = 14`、
     `manifestCoreEpochSelfRenewRejectMax = 14`、
     `manifestCoreEpochSkippedIncompleteMax = 1`、
     receiver `reuse/hold = 0`、`badNonReuseReceiverCount = 0`。
   - Core Epoch 关闭：`identityChurnSamples = 62/134`、
     `identityHashUniqueCount = 33`、
     `manifestCoreEpochUpdatedFromLiveMax = 0`、
     `submittedObjectJaccardMin = 767`、`submittedPartJaccardMin = 791`。
7. Jaccard Min 仍受场景瞬时突增（新对象批量进入）影响，但 Mean/Median 已
   稳定在 `> 990 milli`，远超交接文档 `>= 980` 的可解释门槛；identity churn
   相对基线下降约 51.6%。
8. `AutoTest/phase720_hot_shadow_poll.py` summary 已扩展
   `submittedObjectJaccardMean/Median`、`submittedPartJaccardMean/Median`
   和五个 core epoch 计数器的 `Max` 字段，方便后续回归复核。
9. 未触碰 receiver hold/reuse、shadow TAA/history、alpha/cutout caster 策略、
   direct cap，也未改回 VB/IB capture。
10. 下一步优先级按交接文档排序：检查“不完整对象 part / rigid-looking skinned
    shadows”是否还能被局部修复，再考虑 cutout alpha-test，最后才回到 shadow
    TAA / quality。


2026-05-10 Phase 7.26 + 7.27 性能退烧 + 闪烁收尾后，当前 continuation 继续在
`PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 追加：

1. Phase 7.26 降 Populate CPU：CModel pose probe 按需门控、deferred-index
   miss 快退、material cache key 稳定化、manifest refresh 去除
   O(records × snapshot) 扫描、移除诊断 backing call、core gate 栈上缓冲。
2. Phase 7.27 闪烁收尾：`CoreStalePoseOneFrameRestore` 默认开启并把
   `directLeaseAge` 放宽到 `manifestGeometryCacheFrames`；core gate 软化
   （缺 <= 1 或比例 <= 20% 时允许 partial silhouette 提交，不整 object
   消失）。
3. AutoTest 验收两轮：
   - `phase720_phase727_stale_pose_core_wide[_run2]_hot_shadow_poll_*.json`
   - `phase720_phase727_core_gate_softened[_run2]_hot_shadow_poll_*.json`
   - 两轮一致 `submittedObjectJaccardMin >= 967`、`Mean >= 999`；
     `submittedPartJaccardMin >= 968`、`Mean >= 993.9`；receiver
     `reuse/hold = 0`, `badNonReuseReceiverCount = 0`。
4. 未触碰 receiver hold/reuse、shadow TAA、alpha/cutout caster 策略、
   direct cap，也未回退 VB/IB capture；PoseProbeRestore 仍为关闭默认。
5. 交接回视觉验收阶段；若仍可见残余闪烁，再按 PHASE721 追加里列出的两条
   路线继续（全局调色板通路 / native shadow draw identity probe）。


2026-05-10 Phase 7.28 palette content stability probe 落地后，当前 continuation
继续在 `PHASE721_SLICE_MANIFEST_PROGRESS_2026_05_10.md` 追加：

1. 给 `War3TryBuildLiveRuntimeGroupPalette` 加 `War3SemanticPaletteSource`
   输出枚举和 `paletteSlotIndex` 输出参数；submit 端在每次 skinned submit 上按
   stable `manifestPartLeaseKey` 记录 hash / source / slot / first-matrix
   translation delta / palette count，并把新增 12+ 个 counter 贯通到 bridge /
   hub / control plane / perf / AutoTest。没有改变 submission 行为。
2. 两轮 AutoTest 证据：
   - `AutoTest/artifacts/phase720_phase728_palette_probe_hot_shadow_poll_*.json`
   - `AutoTest/artifacts/phase720_phase728_palette_probe_delta_hot_shadow_poll_*.json`
3. 关键结论：
   - `paletteSourceDrawTimeMax` 占全量，`paletteSourceGlobalSlot / Blended /
     PublishedRegistry / CModelFallback` 全为 0。legacy slot-index fallback
     不是残余闪烁的来源。
   - 同一 stable part 的 `paletteHashChurn = 79..89` / 帧，
     `paletteHashUniqueInWindowMax = 4`，`paletteFirstMatrixLargeDeltaMax = 46`
     （> 1 单位位移），`paletteCountChurnMax = 18`（同 part 帧间骨骼数量
     不同）——这是"snapshot 按 `renderablePart` 地址为 key"+
     "`renderablePart` 每帧换地址"导致跨帧归属错误的强证据。
4. 下一步建议（未开始）：把 skinned palette 的 capture/query key 从
   `renderablePart` 换成 stable part identity（`objectKey + layerIndex +
   payloadWord108`），让 snapshot 真的跨帧追踪同一逻辑 part。CModel
   `FinalPoseMatrixCount/Array` 仍维持 diagnostic-only。
5. 交回给用户决策接下来是否开始 Phase 7.29。


2026-05-10 Phase 7.29 palette attribution key granularity 差分探针落地后：

1. 在 skinned submit 同一点并行采三把 key 的 churn/delta：leaseKey、
   strictSliceKey (+payload11C)，并聚合 leaseKey 下 payload11C 与
   capturedPaletteCount 的 multi-value 证据。
2. 关键产物：
   `AutoTest/artifacts/phase720_phase729_strict_slice_diff_hot_shadow_poll_*.json`
3. 结论：
   - `paletteCountChurnMax` 从 lease 的 18 在 strict 下收敛到 0；
     `paletteLeaseKey{Payload11C,PaletteCount}MultiValueMax` 均为 9。
     这一条说明 paletteCountChurn 证据里有相当一部分是 lease key 把
     不同 cull/variant slice 合并所致，不是 snapshot 错配。
   - `FirstMatrixLargeDeltaMax = 48 (lease) vs 47 (strict)`、
     `HashChurnMax = 91 vs 93` 几乎不变。strict 并不能压掉位移大于 1 的
     跨帧跳变——这是真正的 snapshot attribution 证据，保留在 Phase 7.30+。
4. 明确不做的两件事：
   - 不把 `payload11C` 放回全局 lease/sticky key（会把 Phase 7.23 研究
     已经踢掉的 slice discriminator 硬塞回身份层）。
   - 不直接把 `g_publishedPaletteSnapshotByPart` 的 key 迁到 manifest
     lease key（没有证据证明这是 attribution 错配的主路径）。
5. 留给下一轮决策的两条独立动作：
   - A：给 palette attribution 开一把专用 key
     (`leaseKey + payload11C + capturedPaletteCount`)，只用于 snapshot /
     lease restore 查找，不参与身份；
   - B：在 B 开始前再加一轮 probe，把 strict key 下的 LargeDelta
     与同帧 renderablePart churn 关联起来，确认 snapshot 按
     renderablePart 存是否就是那 47/帧的直接原因。
