# 阶段 C · Q2：Case 29 的**反向变异探针**（补 round 25 的如实记档）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只做探针，无残留改动。**

## 1. 为什么做这个探针

round 25 我如实记档：Case 29 是**回归锁**，但**没有做反向变异** ——
即没有证明「把顺序改成先 Insert 再 CanEmit 会让它失败」。本轮补上。

## 2. 变异方式（最小、可逆、等价可观测）

把 NoteFirstSight 里的 `if (!CanEmit(false)) {` 改成 `if (false && !CanEmit(false)) {`，
即**绕过**预检。这与"先 Insert 再 CanEmit"具有**相同的可观测后果**：
预算已满时仍然插入条目并发射。

（选择"绕过预检"而不是"移动代码块"，是因为它是**单行、可逐字还原**的改动 ——
round 16 我已因多行块删除的边界算错而弄坏过文件。）

## 3. 变异结果（**具名失败**）

```
[BUDGET-29] insertedBefore=64 afterAttempt=65 storedBefore=64 storedAfter=65
FAIL: a budget-refused NoteFirstSight must NOT insert an entry (CanEmit must precede Insert)
FAIL: a budget-refused NoteFirstSight must NOT emit any event
SUMMARY: 28 passed, 1 failed
```

⇒ 被拒的 NoteFirstSight **确实插入了条目**（64→65）且**确实发射了**（64→65）。
⇒ **Case 29 确实由「CanEmit 先于 Insert」这条不变量驱动**，不是空断言。

源码已逐字还原（`restored: true`），随后重建并全量验证。

## 4. 两类入口的不变量现状

| 入口 | 实现顺序 | 回归锁 | 反向变异探针 |
| --- | --- | --- | --- |
| NoteReject | CanEmit 先于 Insert | ✅ Case 25 | 由 round 1 的复查驱动 |
| NoteFirstSight | CanEmit 先于 Insert | ✅ Case 29 | ✅ **本轮完成** |

## 5. 状态

```
Y: emitted=64 terminalEmitted=0 droppedPerFrame=1 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=0 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=64
SUMMARY: 29 passed, 0 failed
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=01230C1F70F6E67B1FEFD8066AFC8521D818852927934214B1DDDE17F330311D
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称 Case 25（NoteReject）也做过同形反向变异 —— 其可失败性来自 round 1 的复查，
  与"我本轮实测过"不是同一件事；
- **不**声称窗口维度已进入查找键；
- **不**声称读方会拒绝链型与**阶段形状**不符（只拦了终态 Recovered）；
- **不**声称实机已验证；**不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。