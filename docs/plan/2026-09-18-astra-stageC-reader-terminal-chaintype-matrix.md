# 阶段 C · 读方：「终态 × 链型」矩阵**封口** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只改读方**，DLL 未变。

## 1. 矩阵的四格

| 链型 | 终态 | 允许？ | 规则 |
| --- | --- | --- | --- |
| Observation | Recovered | ❌ | `observationChainMustNotUseRecovered`（round 24） |
| RejectionRecovery | ObservationClosed | ❌ | **`rejectionRecoveryChainMustNotUseObservationClosed`（本轮）** |
| RejectionRecovery | Recovered | ✅ | 写方：`hasRejectFact && closedChain` |
| Observation | ObservationClosed | ✅ | 写方：链型 Observation 分支 |

⇒ **只有"同型配同终态"被接受**，两个交叉格都被具名拒绝。

## 2. 规则

```python
if (version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION
        and terminal=="ObservationClosed"
        and any(event.get("chainType")=="RejectionRecovery" for event in events)):
    issues.append("rejectionRecoveryChainMustNotUseObservationClosed")
```

**依据**：`ObservationClosed` 的含义是「**观察**已结算」；拒绝恢复链没有观察语义 ⇒
出现它说明链型与终态不符（伪造、或读方串链）。

**⚠️ 按版本门控**：v1/v2/v3 的终态表**不认识** 7（解码处即拒绝），本规则对它们无意义；
显式门控是为了避免「版本无关用法」—— 这是本项目已撞过**三次**的缺陷类
（round 10/11 的 TERMINALS 与 CLOSED_FIELDS、round 28 的 FirstSight 规则）。

## 3. 载荷性（**由变异证明**）

把规则临时改成 `if False:` ⇒ 新用例具名失败；随后规则已逐字还原。

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
DLL=B44F87F6E388284FDF7274EFE4A1F9C3F9C8DE4F1DAC962D8B14A6AB69D11130
分析套件 : 106 tests OK（105 + 新增 test_rejection_chain_must_not_use_observation_closed）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称读方已穷尽「链型 × 阶段 × 终态」的全部组合 —— 本轮封口的是**终态×链型**矩阵；
  「阶段形状 × 链型」只做了两格（Observation+Rejected、RejectionRecovery+FirstSight），
  **其余组合未做**（例如 Observation 带 ServedCandidate 是否合法，取决于归属规则，尚未裁定）；
- **不**声称这些规则在**实机**数据上触发过（它们是判据，不是观察结果）；
- **不**声称窗口维度已进入查找键；**不**声称 K3 生产侧已接线；
- **不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。