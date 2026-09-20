# R1 待裁定问题的解决：由既有契约自证，无需外部裁定 — 2026-09-18

> 承接 `2026-09-18-r1-diff-mechanism-closed.md` §3（"因拒绝而改道替代路径是否为目标行为"）。
> **结论：该行为就是既有已认可的设计语义，R1 与之一致 ⟹ 不改 R1 插入位置，应登记为有意差异。**

## 1. 依据（R1 之前就存在的注释，热缓存路径）

`src/d3d9/war3/semantic/war3_live_palette_selection.cpp:325-332`：

> 「…任一不满足即返回 `0xFFFFFFFFu`，调用方随即跳过下方按 slot 键的 Game.dll 全局 arena /
> QueryBlendedPaletteBySlotIndex 读者，**落到 PoseFallback（PoseRegistry 已发布姿态 +
> producer-owner 反查）这条合法对象的替代路径**。拒绝不写入负缓存，下帧绑定恢复后即可重新命中。」

⇒ 该注释描述的是**热缓存路径**（`useCachedEntry`，`:337-348`）在数量/绑定不满足时的行为，
且**明确把"落到替代路径"称为合法设计**，还说明"拒绝不写负缓存、下帧可重新命中"。

## 2. 推论

| 事实 | 结论 |
| --- | --- |
| 热缓存不足 ⇒ 返回 `0xFFFFFFFF` ⇒ 调用方改道替代路径 | **既有已认可语义** |
| R1 让冷缓存在同类不足时也返回 `0xFFFFFFFF` | **使冷/热一致**，与既有语义同向 |
| 因此出现"改道后经替代路径供出 slot 8"的 11 例 | **是该语义的正常后果，不是缺陷** |

⇒ 因此"因拒绝而改走替代路径"**不需要新的裁定**：它就是该模块既有的、被注释明确认可的行为。
若要求"拒绝即整帧无 palette"，反而会**破坏热缓存路径的既有语义**并使其与冷缓存再次不一致。

## 3. 因此的处理方式

1. **不改 R1 的插入位置**（保持冷/热一致）；
2. **在等价门禁中登记该有意差异**：随机差分世界的比较需要一种机制，使"已声明的意图差异"可见地通过，
   而**未声明的差异仍然失败**；
3. 声明内容应至少包含：差异字段（`chain.slotIndex` / `chain.source` / `chain.hash` / `chain.rawPoseHash` /
   `chain.poseModel` / `chain.paletteWords` / `chain.rejectedDelta` / `chain.phaseCalls` / `random`）、
   允许的两个形态（module 拒绝·legacy 供出；module 经替代路径供出 slot 8·legacy 拒绝）、
   以及本条依据（引用 `:325-332` 的既有契约 + 本次回退实验结论）。

## 4. 诚实边界

- 本条解决的是"**是否为目标行为**"；**不等于**该差异已在门禁中被正确登记（尚未实施）；
- 门禁现状仍为 **7 静态 + 2 meson 失败**；
- 另有 6 个静态失败（`motion_equivalence`、`a9_migration_equivalence`、`m2_4_equivalence`、`m2_5b_equivalence`、
  `taxonomy_equivalence`、`semantic_build_thread_gate_static`）与 meson 的 `war3_palette_object_wire_roundtrip`
  **尚未逐项判定**其是否同一根因；
- 未提交、未部署；现场 DLL 仍是 `F275545B…`；未改用户视频设置；**目标不可标记完成**。