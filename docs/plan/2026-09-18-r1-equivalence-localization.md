# R1 等价门禁失败的精确定位 — 2026-09-18

> 承接 `2026-09-18-r1-equivalence-verdict.md`（回退实验已确证因果）。本文给出**失败发生在哪一段**。

## 1. 失败**不在**固定场景期望表，而在**随机差分世界**

```
M2-4 battery : 7,477,257 checks, 0 failures
A9   battery :   947,458 checks, 0 failures
M2-5B battery: 1,347,808 checks, 0 failures
-- 以上固定场景/系统场景全部通过 --
DIFF line 2181 random:               module=0          legacy=1
DIFF line 2181 chain.slotIndex:      module=4294967295 legacy=4
DIFF line 2181 chain.rejectedDelta:  module=1          legacy=0
DIFF line 2181 chain.source:         module=0          legacy=2
DIFF line 2181 chain.hash/rawPoseHash/poseModel/paletteWords: module=0 legacy=<非零>
```

⇒ 在固定场景表（`CHAIN_EXPECT` / `CHAIN_EXPECT_FLIPPED` / `CHAIN_EXPECT_CONTRACT`）**全绿**的前提下，
差异集中在 20,000 轮**固定种子随机世界**的对照中。

## 2. 差分方向与 R1 的意图**一致**（不是矛盾）

主要形态：`module.slotIndex = 0xFFFFFFFF`（拒绝）而 `legacy.slotIndex = 4`（供出记忆槽位）；
伴随 `rejectedDelta` module=1 / legacy=0，以及 module 的 source/hash/poseModel/paletteWords 全为 0。

这正是 R1 的目标行为：**冷缓存下 producer 的 groupCount 不足以覆盖本次所需矩阵数时，不再供出记忆槽位**。
legacy 参考（pre-M2 快照）没有这条检查，因此在该随机输入上仍然供出 —— 二者必然不同。

## 3. 但**仍有一处未解释**，必须先查清才能动门禁

较早一次运行（只取末尾两行）曾显示**相反**方向：`module.slotIndex=8`、`legacy.slotIndex=0xFFFFFFFF`
（即 module 接受、legacy 拒绝）。若该形态真实存在，则说明 R1 的拒绝会**改走替代路径并最终成功**
（与 `:325-332` 注释所述"落到 PoseFallback 这条合法对象的替代路径"一致），需要在同一输入上确认其归属段落。

⇒ **在把该形态解释清楚之前，不得修改门禁期望**（否则等于"放宽直到通过"）。

## 4. 为什么不能简单改期望表

- 固定场景表已全绿 ⇒ 需要声明差异的是**随机世界的比较**，它**没有**逐场景期望表；
- 因此需要新增的是一种**有意差异声明机制**（例如：按字段/形态登记的已知意图差异，或把"必须逐位相同"收窄为"除已声明差异外必须逐位相同"），并附原因引用；
- 这正是外部复审要求的"有版本/显式"处理方式，而不是把随机世界的比较整体关闭。

## 5. 结论与下一步

| 项 | 状态 |
| --- | --- |
| R1 与失败门禁的因果关系 | **已确证**（回退实验双向可复现） |
| 失败段落 | **已定位**：随机差分世界（固定场景表全绿） |
| 主要差分方向 | **与 R1 意图一致**（module 拒绝 / legacy 供出） |
| 反向形态（module 供出 / legacy 拒绝） | **未解释，必须先查清** |
| 门禁期望 | **尚未修改**（不得在查清前放宽） |

下一步：在同一随机输入上定位反向形态的归属（是否走替代路径后成功），确认后再设计有意差异声明机制。

## 6. 边界

未提交、未部署；现场 DLL 仍是 `F275545B…`；未改用户视频设置；**目标不可标记完成**。
门禁现状：7 静态 + 2 meson 失败（未恶化、未通过）。