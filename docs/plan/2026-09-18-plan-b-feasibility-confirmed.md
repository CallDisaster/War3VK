# 方案 B 可行性已确认：生成器快照齐备 — 2026-09-18

> 承接 `2026-09-18-divergence-registration-spec.md` §2 方案 B。本文确认其**可复现性前提已满足**，并给出细化的实施形态。

## 1. 前提检查：迁移前快照**全部在位**

生成器 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py` 的输入是 `%TEMP%` 下的迁移前 device.cpp 逐字节副本（见其 docstring `:18-26`）。实测：

```
%TEMP% = D:\tmp\AppData\ADMINI~1\Local\Temp
PRESENT device_pre_m2_1.cpp   2,314,463 B  2736335B42FCF494
PRESENT device_pre_m2_2.cpp   2,310,648 B  B70F3AF97CD3DB55   <-- chain 参考的输入
PRESENT device_pre_m2_3.cpp   2,278,494 B  ECC4B828AF344DCF
PRESENT device_pre_a9.cpp / a10 / m2_4 / m2_5 / m2_5b 亦在位
```

现有参考文件（含被门禁钉死的 SHA）：

```
war3_live_palette_selection_legacy_reference.inc          7,702 B  D458C1AE6FF08B49
war3_live_palette_selection_chain_legacy_reference.inc   36,025 B  02FF8AFE5A30E371  <-- R1 涉及
war3_live_palette_selection_motion_legacy_reference.inc   8,406 B  7DF61688219BAF83
war3_live_palette_selection_a9_legacy_reference.inc       3,026 B  D92048FCB8B293C2
war3_live_palette_selection_m2_4_legacy_reference.inc     9,397 B  E9CB8931B427CB87
war3_palette_taxonomy_emission_legacy_reference.inc      22,511 B  A5333AC7E7F5A080
war3_device_semantic_predicates_legacy_reference.inc     43,452 B  2EA43F9782BE388E
war3_palette_submitted_aggregation_legacy_reference.inc   4,952 B  C6D32B4EF8DCCC27
```

⇒ **方案 B 的可复现性前提已满足**：参考可由快照确定性重建与审计。

## 2. 细化的实施形态（两个子方案，需择一）

R1 是**迁移之后**的变更，因此"直接用快照重建"只会得到**不含 R1** 的参考（等于现状）。要把新契约写进基线，只有两条路：

### B1：**把 R1 作为生成器的显式、带版本的补丁步骤**（推荐）

在生成器中新增一个**声明式补丁阶段**：对 `--m2-2` 抽取出的 chain 本体，应用一条具名补丁
（例如 `POST_MIGRATION_CONTRACT_PATCHES = [("R1-cold-cache-groupcount", <定点替换>, <依据>)]`），
再输出 `.inc`。

**优点**：
- 参考仍**确定性可重建**（快照 + 具名补丁），审计者可复算；
- 差异是**显式登记**的（补丁名 + 依据 + 版本），不是白名单放行；
- `DiffU64` 的逐位等价比较**强度不变**；
- `.inc` 头部 provenance 记录"含哪条迁移后补丁、依据哪次裁定"。

**代价**：生成器与 `.inc` 均变更 ⇒ 需同步更新**钉死 SHA 的门禁**（见 §3）。

### B2：手工修改 `.inc` 并更新 SHA

**缺点**：破坏"从快照确定性重建"这一可审计属性（手改内容无法由生成器复算）⇒ **不推荐**。

## 3. 无论 B1/B2，都必须同步的三处

1. `.inc` 内容 + 其头部 **provenance**（记录 R1 补丁与依据引用）；
2. **钉死 SHA 的门禁**（chain `.inc` 现为 `02FF8AFE5A30E371…`）—— 5 个静态门禁各自重复了该断言，需逐一同步；
3. 门禁中与"参考来源"相关的文字说明（避免与新的基线含义矛盾）。

## 4. 但仍**不得跳过**步骤 ①

外部复审要求的顺序是"**先证明新行为符合预期**，再处理等价基线"。因此本轮**没有**开始 B1：