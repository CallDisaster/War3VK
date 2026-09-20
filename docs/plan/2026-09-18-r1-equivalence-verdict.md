# R1 与等价门禁：定论（回退实验）— 2026-09-18

> 结论：**R1 是有意行为修复，等价门禁的期望必须更新；不回退 R1。**
> 本文取代上一份文档中「差分方向与预期相反、原因未判定」的未决状态。

## 1. 决定性实验（回退法）

| 步骤 | 操作 | 结果 |
| --- | --- | --- |
| A | 保留 R1（现状） | `test_war3_live_palette_selection_equivalence_static.py` **EXIT=1（失败）** |
| B | **临时回退 R1**（仅删掉冷缓存数量检查，改为原 `currentSlotIndex = producerSlotIndex;`） | 重新构建 `war3_live_palette_selection_test.exe` 后同一门禁 **EXIT=0（static checks passed）** |
| C | **恢复 R1**（并把本发现写入代码注释） | 重新构建后门禁 **EXIT=1（复现）** |

⇒ **R1 与该门禁失败之间的因果已确证**（B/C 双向可复现）。

## 2. 排除项（不是这些原因）

- **不是契约 env**：`DXVK_WAR3_SKIN_PALETTE_CONTRACT` 取 `0` / `1` / 不设置，三种状态下门禁**同样失败**；
- **不是批次 3（FirstSight）或 R2/R3**：那些改动位于证据记录器与设备采集点，与本选择链差分无关（且回退 R1 即通过）。

## 3. 机制（为什么"只加拒绝"会改变可观测行为）

R1 的检查位于冷缓存分支：

```cpp
if (currentSlotIndex == 0xFFFFFFFFu || currentSlotIndex >= 0x3A98u) {
  const uint32_t producerSlotIndex = queryProducerBindingSlot(&producerGroupCount, &producerFrameTag);
  if (producerSlotIndex == 0xFFFFFFFFu) return currentSlotIndex;
  if (producerGroupCount < requiredPaletteCount) { ...; return 0xFFFFFFFFu; }   // R1
  currentSlotIndex = producerSlotIndex;
}
```

**返回值 0xFFFFFFFF 不是终态，而是"路由信号"**：调用方（`:408` 起）据此跳过按 slot 键的 arena 读取，落到后续**合法的替代路径**（part snapshot / published pose 等；见 `:325-332` 既有注释「落到 PoseFallback 这条合法对象的替代路径」）。
因此"多拒绝一次"会**把该次选择改走另一条路径**，从而产生不同的最终 slot 与不同的 served/rejected 计数 —— 差分实测正是如此：

```
chain.slotIndex:     module=8          legacy=4294967295
chain.servedDelta:   module=1          legacy=0
chain.rejectedDelta: module=0          legacy=1
```

⇒ 差分**方向"相反"并非矛盾**：module 拒绝了**冷缓存**这条路，随后**经替代路径成功**，因此表现为"served"；legacy 则在冷缓存直接返回 0xFFFFFFFF、未再尝试替代路径。
**我此前"R1 只可能多拒绝、不可能把拒绝变成接受"的推理，错在把返回值当成了终态，而它实际是路由信号。**

## 4. 应当怎么做（按外部复审已给定原则）

复审原则：「搬代码时，应证明新旧行为相同；**修错误时，应证明新行为符合预期。不能要求修复继续与旧错误完全等价。**」

⇒ **不回退 R1**。正确顺序是：

1. **先证明新行为符合预期**：用独立测试固定"冷缓存 groupCount 不足 ⇒ 拒绝并走替代路径；数量足够 ⇒ 与热缓存同结果"（本项可加进已建的 `war3_skin_palette_admission_test` 或选择链探针）；
2. **再为有意差异建立显式声明机制**：在等价门禁中登记该差异（场景 id + 预期方向 + 原因引用），使它**可见地**通过，而不是让门禁静默变红；
3. **其余失败项逐一判定**：另 6 个静态失败（`motion_equivalence`、`a9_migration_equivalence`、`m2_4_equivalence`、`m2_5b_equivalence`、`taxonomy_equivalence`、`semantic_build_thread_gate_static`）与 meson 的 `war3_live_palette_selection` 需逐项确认是否同一根因；`war3_palette_object_wire_roundtrip` 在 meson 下失败但**显式设置两个 env 后为 116/116 PASS**，疑为 env 注入差异，需单独判定。

## 5. 当前树状态

- **R1 已恢复**（未丢失修复），并在代码内以注释记录了本定论；
- `ninja -C build32` 已全量重建（`d3d9.dll` 与 `war3_live_palette_selection_test.exe` 均已重链）；
- 门禁现状：**7 静态 + 2 meson 失败**（与上一份文档一致，未恶化）；
- 未提交、未部署；现场 DLL 仍是 `F275545B…`；未改用户视频设置。