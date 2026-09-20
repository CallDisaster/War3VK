# 阶段 C · K3：机制变异探针（证明 Case 31 载荷）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只做探针，无残留改动。**

## 1. 为什么做这个探针（纠正 round 39 的一处推理）

round 39 我在文档里写："Case 30 相当于对照"（未知哨兵 ⇒ 既有行为 ⇒ Unclosed），
并据此认为机制的效果已被差分证明。

**但那只是"同一个二进制的两条不同路径"，不是"改掉机制后它会失败"的实测。**
本轮补上真正的变异探针。

## 2. 变异方式（单行、可逐字还原）

把 `MarkStage` 里的 `if (attemptSerial == ~0ull) {` 改成 `if (true) {` ——
即**无论尝试号是否已知，都走既有终身单调分支**。

（同样选择"单行可逆"的改法，避免 round 16 那种多行块删除边界算错。）

## 3. 变异结果（**具名失败**，且只失败一条）

```
[K3-31] twoAttemptsDistinctSerial terminal=Unclosed closedRecovered=0
FAIL: two distinct attempt serials must NOT be an order violation
      (the second attempt is legitimate, so the entry must settle as Recovered)
[K3-31] sameSerialRollback terminal=Unclosed
SUMMARY: 30 passed, 1 failed
```

⇒ **第一条断言具名失败**（不同尝试号被误判违规）；
⇒ **第二条仍然通过**（同尝试号内回退依旧违规）。

**这一点本身有信息量**：两条断言由**不同**机制驱动 ——
第一条依赖"已知尝试号触发重置"，第二条依赖"既有违规判据仍在"；
变异只打掉前者 ⇒ 二者不是同一个检查的两种说法。

源码已逐字还原（`restored: true`），随后重建并全量验证。

## 4. 与 round 39 的关系

| 说法 | 性质 |
| --- | --- |
| round 39："Case 30 是对照" | **推理**（同一二进制两条路径） |
| 本轮：变异后第一条断言失败 | **实测** |

两者结论一致，但**证据等级不同**。本轮的才是"可失败性"证明。

## 5. 状态

```
d
[COUNTERS] shared-recorder snapshot before SUMMARY: emitted=6 terminalEmitted=1 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=0 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=1 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 31 passed, 0 failed
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=B44F87F6E388284FDF7274EFE4A1F9C3F9C8DE4F1DAC962D8B14A6AB69D11130
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称 K3 已完整落地 —— **生产调用方仍未给 `attemptSerial`**，实机判序仍是终身单调；
- **不**声称"一次尝试"的语义已定；
- **不**声称读方需在 wire 见 `attemptSerial`（未扩 v4 字段集）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。