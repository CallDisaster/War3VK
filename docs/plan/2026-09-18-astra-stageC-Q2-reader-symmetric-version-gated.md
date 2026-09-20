# 阶段 C · Q2 读方：**对称规则 + 版本门控** —— 拒绝恢复链不得携带 FirstSight — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**只改读方**，DLL 未变。

## 1. 补齐对称缺口

round 27 我如实记档：「拒绝链携带 FirstSight」**未做**。本轮补齐。

```python
if (version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION
        and any(event.get("chainType")=="RejectionRecovery" for event in events)
        and any(event["stage"]=="FirstSight" for event in events)):
    issues.append("rejectionRecoveryChainMustNotCarryFirstSightStage")
```

**依据**：拒绝恢复链由 NoteReject 建立，FirstSight 是**观察链**的链首 ——
一条链不该同时拥有两类链首。

## 2. ⚠️ 为什么**必须**按版本门控（本轮的关键点）

**v3 认识 `FirstSight` 却*没有*链型载体**（解码处恒给 RejectionRecovery）。
若不加门控，读方会把**合法的 v3 观察链**判成"拒绝恢复链带了 FirstSight" ⇒ **误拒**。

这正是本项目反复出现的「**版本无关用法**」缺陷类（round 10/11 在 TERMINALS 与
CLOSED_FIELDS 上各撞过一次）。因此本轮**显式登记**门控，并用**两侧断言**钉住：

| 版本 | 同一形状（FirstSight 链首） | 期望 |
| --- | --- | --- |
| v4 | 拒绝恢复链（chain_type=0）带 FirstSight | **必须具名判错** |
| v3 | FirstSight 链首（无链型载体） | **不得**判错（合法观察链） |

## 3. 载荷性（**两个变异探针**，各证一件事）

| 探针 | 变异 | 结果 | 证明了什么 |
| --- | --- | --- | --- |
| **A** | **去掉版本门控** | `... unexpectedly found` ⇒ FAILED | **门控本身载荷**（它真的在防 v3 误判） |
| **B** | 规则恒假（`if False`） | `... not found` ⇒ FAILED | **规则本身载荷** |

**为什么两个都要**：只做 B 无法区分"门控生效"与"门控没写"；
只做 A 无法证明规则本身在起作用。两个探针**分别**排除这两种可能。
源码已逐字还原。

## 4. 状态

```
=== 读方 ===
OK
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=01230C1F70F6E67B1FEFD8066AFC8521D818852927934214B1DDDE17F330311D
分析套件 : 105 tests OK（104 + 新增 test_rejection_chain_must_not_carry_first_sight_version_gated）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 读方链型规则现状

| 规则 | 拦什么 | 版本门控 | 探针 |
| --- | --- | --- | --- |
| observationChainMustNotUseRecovered | 链型 × 终态 | 不需要（v1-v3 无链型载体且无 Observation） | 单探针 |
| observationChainMustNotCarryRejectedStage | 链型 × 阶段形状 | 不需要（同上） | 单探针 |
| rejectionRecoveryChainMustNotCarryFirstSightStage | 链型 × 阶段形状（对称） | **必需**（v3 认识 FirstSight） | **A+B 双探针** |

## 6. 不声称

- **不**声称读方已穷尽链型/形状组合（例如"观察链带 Enqueued 而无任何前序"**未做**）；
- **不**声称这些规则在实机数据上触发过；
- **不**声称窗口维度已进入查找键；
- **不**声称实机已验证；**不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。