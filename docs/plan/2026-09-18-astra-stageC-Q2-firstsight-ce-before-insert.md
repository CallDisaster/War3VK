# 阶段 C · Q2：裁定不变量上锁 —— **NoteFirstSight 先 CanEmit 再 Insert** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只加测试**（未改产品源码语义）。

## 1. 裁定要求的现状核查

裁定：「**两类入口都必须先 CanEmit 再 Insert**」。

| 入口 | 实现顺序 | 是否上锁 |
| --- | --- | --- |
| NoteReject | 去重 → 表满 → CanEmit 预检 → Insert → 发射（round 1 已改） | ✅ Case 25 |
| **NoteFirstSight** | 去重 → CanEmit → Insert → 发射（**实现本就正确**） | ❌ **未上锁** |

核查发现：`firstSightInserted` 在测试里**只有正向断言**（`== 1u`，两处），
**没有**「被预算拒绝时 `firstSightInserted` 不得增长」的断言。
⇒ 实现正确，但**没有任何东西阻止它将来退回**（正是 round 1 在 NoteReject 上遇到的问题）。

## 2. 新增 Case 29（Case 25 的对称件，2 checks）

- 同一帧内用 **64 个不同对象键**把每帧非终态预算（`kPerFrameBudget == 64`）打满；
- 再对**第 65 个全新对象**调用 `NoteFirstSight`；
- 断言：`firstSightInserted` **不得增长**（不得留下无头条目）且 `g_stored` 不得增长（不得发射）。

实测：

```
[BUDGET-29] insertedBefore=64 afterAttempt=64 storedBefore=64 storedAfter=64
droppedPerFrame=1  watchCount=64
```

⇒ 第 65 条被每帧预算拒绝（`droppedPerFrame=1`），且插入数与发射数**都停在 64**
⇒ **先 CanEmit 再 Insert 在 NoteFirstSight 上成立**。

## 3. 状态

```
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=9CB3AA3C91172A7607A4DA86FC36BE44A044BE94CE70E2977EC7086D59D5CC87
evidence : 29 passed, 0 failed（Case 29 = 2 checks）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 4. 不声称

- **不**声称本轮的探针是"变异探针"：Case 29 是**回归锁**，我没有把实现改成
  「先 Insert 再 CanEmit」去实测它会失败（**未做，如实记档**）；
  它与 Case 25 同型，二者共同覆盖两类入口；
- **不**声称窗口维度已进入查找键；
- **不**声称读方会拒绝链型与**阶段形状**不符（只拦了终态 Recovered）；
- **不**声称实机已验证；**不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。