# 阶段 C · 窗口维度：**从"违规不可观测"到"违规可见"** — 2026-09-18

> 本文**接续并修订** `2026-09-18-stageC-window-dimension-unobservable-under-violation.md`。
> round 60 的结论（"违规时也不可观测 ⇒ 需要裁定"）**已被本轮的实现推翻**：
> 在**不触 wire** 的前提下，该违规**可以**变成可见的。

## 1. round 60 我漏掉的一点

round 60 我断言"没有任何导出层检查能发现它" ⇒ 结论是"需要裁定 A/B/C"。
但那个断言只说对了一半：**导出层**确实看不出来（`record.windowSegment` 取自当前值），
可是**记录器内部**可以看出来 —— 只要查找时**知道**条目属于哪个窗口。

⇒ 关键在于：把窗口放进查找谓词之后，"保留条目"的变异会让查找**遇到别的窗口的条目**，
   而这个事件**可以在内部计数**。**不需要改 wire。**

## 2. 实现（三处，全部在记录器内部）

  ① `Entry::windowSegment`（建立时落定，与 `chainType` 同处盖章）；
  ② `FindChain` 的匹配谓词加入窗口：同 key+链型 但**别的窗口** ⇒ **不复用**，
     并 `++m_counters.windowMismatchLookups` 后 `continue`（继续探测）；
  ③ `Counters::windowMismatchLookups` —— **故意不写入 wire**（裁定要求新写方一律 v4；
     加一个导出的计数器会触版本表，见 round 10 的 `CLOSED_FIELDS` 陷阱）。

## 3. 探针（**两次**，第二次才成立 —— 过程如实记录）

| 变异 | 结果 | 我学到什么 |
| --- | --- | --- |
| **只删 `m_entries` 清空循环**（保留清 bloom 与清计数） | **仍 PASS** | bloom 被清 ⇒ `FindChain` 在 bloom 门**提前返回**，**根本扫不到旧条目** ⇒ 这不是"跨窗口保留" |
| **完整保留（entries + bloom + 计数，只递增段号）** | ★ **FAIL（具名）** | 这才是真正的"跨窗口保留条目" ⇒ 查找**遇到别的窗口的条目** ⇒ 计数 > 0 ⇒ 断言失败 |

**第一个变异的"仍然通过"本身是有信息量的**：它说明"保留条目"若同时清了 bloom，
造成的不是"跨窗口合并"，而是**孤儿条目占槽**（另一个缺陷，不在本维度内）。

## 4. 这条守卫的**准确界限**（不夸大）

- 它守卫的是「**查找**不会遇到别的窗口的条目」；
- 它**不是**导出的失败可见性：`windowMismatchLookups` **不上 wire** ⇒
  **实机上的这类违规仍然只出现在记录器内部**（需调试/测试才能看到）；
- 它**可失败**：完整"跨窗口保留"变异会让 Case 34 **具名失败**（已实测）；
- 因此它把 round 60 的"**无任何守卫**"变成了"**有守卫，但只在记录器内部可见**" ——
  这比"需要裁定才能有守卫"要好，但**仍不是** wire 级的 fail-visible。

## 5. 状态

```
son ===
Ok:                85
Fail:              0
[COUNTERS] shared-recorder snapshot before SUMMARY: emitted=1 terminalEmitted=0 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=0 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 33 passed, 0 failed
wire=0
CHECKS=1160 FAILURES=0
=== 读方 ===
OK
site=F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
DLL=B3D08D6D9DE9A044F03192662403BE58B6DBA1D0AA49B55F377D2D5CDB176ED3
h evidence.h = CD1D0540E708D8E1BE7ACFCF2B8AC00315FDE3FEF43B6932A5AD7623E424F129
站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（与基线逐字相同 ⇒ 未部署）
git  = 无写操作
```

## 6. 仍然需要裁定的**剩余**部分

若要让这类违规在**导出层**也 fail-visible（实机排查用），仍需裁定：
the wire 是否抬到 v5（把窗口段放进条目并导出），或是否把 `windowMismatchLookups`
作为 v4 counters 的**新增字段**导出（后者与"新写方一律 v4 + 版本表"冲突，需裁定）。

## 7. 不声称

- **不**声称窗口维度在**导出层**可观测（**恰恰相反**：计数不上 wire）；
- **不**声称实机上出现过该类违规（是**假设的变异**，非观测）；
- **不**声称"保留条目"一定会导致跨窗口合并（第一种变异表明它还可能是**孤儿占槽**）；
- **不**声称 C 阶段完成（这一条现在有**记录器内部**守卫，导出层仍需裁定）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。