# 目标③ 达成评估（按目标原文的六项约束逐条取证）— 2026-09-18

目标③ 原文：

> 设计并实现**有版本**的「正常观察链」（首次观察→候选入队→draw已记录→窗口结束），
> 保留既有拒绝恢复链，写方/解析器/测试同步更新；不伪造 Rejected、不让 NoteEnqueued
> 自动插表后再放宽解析器、不把未知身份升级为已证明。

## 逐条取证

### ① 有版本的正常观察链

- 写方：`palette-object` wire 版本 3（`war3_palette_object_evidence.h` 的 version 3 块，含
  `firstSightInserted` / `firstSightEmitted`）；
- 读方：`analyze_palette_object_evidence.py` 的 `PALETTE_OBJECT_FIRST_SIGHT_VERSION = 3`，
  且 `PALETTE_OBJECT_BLOCK_FIELDS` 同时登记 v1/v2（拒绝恢复）与 v3；
- 阶段枚举含 `Rejected` / `ServedCandidate` / `Enqueued` / `Drawn` / `FirstSight`（秩 0 = 链首）；
- 实机：正常链已被解析器**正面认定**（`stageObservationComplete` 64/64，见 round 259 记录）。

**✅ 成立。**

### ② 保留既有拒绝恢复链

- `NoteReject` 路径未改（`:299-307`），仍以「真的发生了拒绝」为建条目依据；
- 版本门控：v1/v2 的 `LEGACY_STAGES`（1..4）不含 `FirstSight`；遇 stage=5 时**整份拒绝**
  （有行为测试覆盖：v in {None,1,2} 拒、v=3 受）；
- 终态回填对拒绝恢复链**保持 `Drawn`**（`:264-269` 注释明示「版本 1/2 冻结行为，一位不改」）。

**✅ 成立。**

### ③ 写方/解析器/测试同步更新

- 写方：首见按条目去重（修正①）、预算预检提前（修正②）——均已在同一 wire 版本内，无字段/版本变更；
- 解析器：`LEGACY_STAGES` + 版本选表、deltaFrames 分支修正、完成判据按链型、两条计数器单向不变量；
- 测试：静态 98 个（含链首整份拒绝、多帧 delta、链首前有事件必须阻断、计数器不变量）；
  wire roundtrip `CHECKS=1137 FAILURES=0`；meson 85 Ok / 0 Fail；静态全量 259 / 0 failed。

**✅ 成立。**

### ④ 不伪造 Rejected

- `NoteFirstSight` 的 `firstReason` 记 `PaletteObjectRejectReason::NotChecked`（`:417` 附近），
  表示「本链没有拒绝事实」；
- 源码注释明示：首见**不是**拒绝，**不得**伪装成 `Rejected`，也不得让读方解释成「已恢复」；
- 实测导出中首见链的 `Rejected` 事件数为 0（阶段分布只有 `FirstSight`/`Enqueued`/`Drawn`）。

**✅ 成立。**

### ⑤ 不让 NoteEnqueued 自动插表后再放宽解析器

- 本轮实测：`Insert(` 在全文件中**只**出现在 `:307`（`NoteReject`）、`:417`（`NoteFirstSight`）
  与 `:845`（`Insert` 定义本身）；
- ⇒ `NoteServed`（`:445`）、`NoteEnqueued`（`:491`）、`NoteDrawn`（`:536`）、`NoteObjectGone`（`:582`）
  **都不插表**，只 `Find` 已有条目；
- ⇒ 解析器的「链首必须存在」不变式**未被放宽**（其守卫测试
  `test_event_before_the_chain_head_must_block_completion` 仍然生效）。

**✅ 成立。**

### ⑥ 不把未知身份升级为已证明

- `nativeKnown` 在生产采集点固定为 `false`（`d3d9_device.cpp:23520` 的 `0u, false`，见目标②(b)）；
- D 点的 native 已知性由 `frameTag != 0` 决定（`d3d9_war3_shadow.cpp:5245-5246`），`frameTag == 0` 时
  同时记 `nativeUnknown=true` 与 `selectionClearedByNativeOverride=true`；
- `epochUnknown` / `deviceEpoch` 语义未被提升；wire 往返驱动 G 场景原先**声称**夹具与生产一致，
  实际生产身份形状（`epochUnknown=true` + `deviceEpoch=0`）**未被覆盖** —— 该**假声明已被更正**，
  且被如实记为「生产身份形状零覆盖」的已知缺口（未用"已证明"掩盖）。

**✅ 成立**（就"不升级"而言）；但**生产身份形状的覆盖缺口**仍是未完成的质量项（见下）。

## 结论

**目标③ 的六项约束全部满足。**

因此我判定：**③ 已按目标原文达成**。

## 未完成项（**属于我自加的扩展清单，不属于目标原文**）

| 项 | 性质 |
| --- | --- |
| `FirstSight` 正名（其真实语义是「首次进入该生产路径」） | 命名与文档质量；需协调写方/两读方/测试，wire 值不变 |
| G 门禁三函数参数化（`check_decoded_events` / `check_gaps` / `check_chain`） | 覆盖扩展；已有 6 处失败清单 |
| 夹具改真生产形状（`epochUnknown=true` + `deviceEpoch=0`） | 覆盖扩展；会牵动 A-G 多处断言 |

**这三项不阻塞 ③ 的达成判定**，但**不应被说成"已完成"**。

## 现场

```
站点     : E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
候选 DLL : 36,289,280 B  SHA-256 5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D
本轮未改任何文件（只读代码）。未提交、未部署。
```