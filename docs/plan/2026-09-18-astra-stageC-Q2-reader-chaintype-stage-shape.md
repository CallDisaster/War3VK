# 阶段 C · Q2 读方：观察链**不得携带 Rejected 阶段**（阶段形状拒收）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**只改读方**，DLL 未变。

## 1. 补齐一条我两次标记的缺口

round 24 我加了「Observation 永不使用 Recovered」，并如实记档：
**读方只会拒绝"链型与终态不符"，不会拒绝"链型与阶段形状不符"**。本轮补齐后者。

## 2. 规则

```python
if (any(event.get("chainType")=="Observation" for event in events)
        and any(event["stage"]=="Rejected" for event in events)):
    issues.append("observationChainMustNotCarryRejectedStage")
```

**依据**：观察链由 NoteFirstSight 建立，其条目**没有**拒绝事实（两类链各自独立条目，
round 19/22 已落地）。因此观察链里出现 Rejected 只有两种可能 ——
**伪造**，或**读方把两条链串成了一条**。两种都必须具名判错。

**与上一条的关系**：Recovered 拦的是**终态**，本条拦的是**阶段形状**；两者互补，不是重复。

**v1/v2/v3 天然不受影响**：那三个版本无链型载体（解码恒给 RejectionRecovery）。

## 3. 载荷性（**由变异证明**）

把规则临时改成 `if False:` 后重跑分析套件 —— 新用例失败（具名）；随后规则已逐字还原。

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
分析套件 : 104 tests OK（103 + 新增 test_observation_chain_must_not_carry_rejected_stage）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 读方链型规则现状

| 规则 | 拦什么 | 状态 |
| --- | --- | --- |
| observationChainMustNotUseRecovered | 链型 × **终态** | ✅ round 24 |
| observationChainMustNotCarryRejectedStage | 链型 × **阶段形状** | ✅ 本轮 |

## 6. 不声称

- **不**声称读方已拒绝**全部**类型的链型/形状不符（例如"拒绝链携带 FirstSight"**未做**）——
  本轮只做了观察链不得带 Rejected 这一条；
- **不**声称这些规则在实机数据上触发过（它们是判据，不是观察结果）；
- **不**声称窗口维度已进入查找键；
- **不**声称实机已验证；**不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。