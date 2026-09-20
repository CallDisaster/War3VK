# 阶段 B 完成：门禁恢复为"可以失败且失败具名" — 2026-09-18

> 本文件属 **Astra 复核后续计划**（Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`）。
> 顺序：A 文档更正版（已完成）→ **B 门禁恢复（本文件）** → C 链型/v4 → D 会话生命周期 → E 受控实机。

## 1. 缺陷（复核 P0）

`AutoTest/test_palette_object_wire_roundtrip.py`：

| # | 缺陷 | 位置 |
| --- | --- | --- |
| D1 | `check_scenario_g` **从未被调用** —— 调用写在同一行注释里 | `:1161`（原文 `# … spec_results["G"] = check_scenario_g(data)`） |
| D2 | 终判只统计 `spec_results` 里**现有**项，**不要求 A–G 齐全** ⇒ 删掉一个场景的求值即可"通过" | `:1172-1173` |
| D3 | 失败只打印**计数**，从不打印**内容** ⇒ 门禁失败也不说为什么 | `:1212-1214` |

## 2. 改动

| # | 改动 | 位置 |
| --- | --- | --- |
| F1 | 把 `spec_results["G"] = check_scenario_g(data)` **移出注释**，并写明这段历史（避免再次被注释掉） | 原 `:1161` 之后 |
| F2 | 新增 `REQUIRED_SCENARIOS = frozenset(SCENARIO_ORDER)`；终判前强制"必需场景齐全"，缺失时**具名**追加 FAILURE | `:327` 后、`:1172` 前 |
| F3 | `FAILURES` 逐条以 `FAILURE[i/n] <内容>` 打印 | `:1212` |

**未做**：没有放宽任何期望、没有删除任何断言、没有把错误期望登记为通过。**G 的 4 条断言一条未改。**

## 3. 验证

### 3.1 基线（恢复正常）

```
exit=0  CHECKS=1141 FAILURES=0
SPEC_SATISFIED=['A','B','C','D','E','F','G'] SPEC_UNMET=[]
ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED
```

⇒ 恢复 G 的调用使 `CHECKS` 由 **1137 → 1141**，**正好 4 条**，
与复核指出的"`check_scenario_g` 含四个 `check()`"**逐条吻合**。
（这也解释了我此前记录里"1141 降到 1137、差 4 无法解释"的那 4 条。）

### 3.2 可失败性探针（4/4 全部使门禁失败）

| 探针 | 变异 | 结果 |
| --- | --- | --- |
| P1 | 删除 G 的求值 | `exit=1 CHECKS=1137 FAILURES=1`；`FAILURE[1/1] required scenarios missing from spec_results (never evaluated): ['G']` |
| P2 | G 的 `chains == 1` 反向为 `== 999` | `exit=1 CHECKS=1141 FAILURES=1` |
| P3 | G 的 `expected_version=3` 改为 `2` | `exit=1 CHECKS=1141 FAILURES=1` |
| P4 | G 的 `not result["recovered"]` 反向 | `exit=1 CHECKS=1141 FAILURES=1` |

**P1 是本次修复的核心证明**：在修复前，"把某个场景的求值删掉"**不会**产生任何失败
（`spec_results` 少一项而已）；修复后它**具名失败**。

（探针在**副本**上执行，正本未被变异；副本已删除，`AutoTest` 下无残留 `_probe_*`。）

### 3.3 未回归

```
AutoTest 全量静态 : 259 scripts, 0 failed
```

## 4. 边界（不得越过）

- **本阶段只改测试驱动**（`AutoTest/test_palette_object_wire_roundtrip.py`），**未改任何产品源码、未重建 DLL**；
- 因此**候选 DLL 与阶段 A 相同**（`5ADEDD5F…`），本阶段**不产生新候选**；
- 本阶段**不**声称"正常链写读契约已修" —— 那属于阶段 C（链型 / v4 / `ObservationClosed`）；
  G 现在能通过，**只说明 v3 的 header 形状与那 4 条 G 断言成立**，
  不说明 K1–K3（误拒不变量、未拒绝链被标 `Recovered`、混合链与跨帧语义）已解决；
- `check_decoded_events` / `check_gaps` / `check_chain` 对 G 的**链型参数化**仍未做
  （原记录的 6 处差异仍待逐条判定"读方坏还是夹具坏"），属阶段 C。

## 5. 现场

```
工作树 : dxvk-v1.22-integration-20260914
候选   : build32\src\d3d9\d3d9.dll  36,289,280 B  5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D（未变）
站点   : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git    : 无写操作
```
