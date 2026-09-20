# 阶段 C · 窗口维度：**比"真空满足"更糟 —— 即使被违反也不可观测** — 2026-09-18

> 这是对 round 45 结论的**修正**。round 45 我写「查找侧真空满足」；本轮实测表明，
> 窗口维度不仅"当前没有可观测差异"，而是**在违规时也无法从导出里看出来**。

## 1. 本轮做了什么（以及为什么必须做）

round 45 我把窗口维度判为"真空满足"并回退了显式实现。但"真空满足"这个说法**没有经过
违规情形的检验**：我只证明了"当前实现下没有差异"，没有证明"违规时能被发现"。

于是我写了 Case 34，先断言 re-arm 必须清表，再加一条**载荷断言**：

  re-arm 后同一 key 的**新事件**必须带**新**窗口段（若条目被保留，旧条目会被复用）

## 2. 探针结果：**载荷断言没有失败**

变异：把 `ResetForSessionTransition` 的**清表循环删掉**（只保留 `m_watchCount = 0u`），
即"**保留条目、只清计数**"。

  [W34] stored=1 lastSeg=2    PASS

⇒ 事件**依然带新段号 2**。

## 3. 为什么（根因）

`record.windowSegment` 是在**建记录时**从 `m_windowSegment`（**当前值**）盖上去的
（`war3_palette_object_evidence.h` 内 `record.windowSegment = m_windowSegment;`），
**不是**从条目里读的。

⇒ 即便复用了**窗口 1 的旧条目**，事件依然带**当前**段号 2。
⇒ **窗口段号字段根本不能用来判别"条目属于哪个窗口"。**

## 4. 这个发现的意义（比 round 45 更强）

| 说法 | 强度 |
| --- | --- |
| round 45：「查找侧**真空满足**」 | 只说"当前无可观测差异" |
| **本轮：「违规时**也不可观测**」** | 说"**没有任何导出层检查能发现它**" |

⇒ 若日后有人把 re-arm 改成"保留条目"，会产生一个**静默**的窗口维度违规：
  同一个对象跨两个窗口的观察会被**合并进同一条链**，而导出里**看不出任何异常**
  （所有事件都带当前段号，读方的分组键也照常工作）。

## 5. 我因此**移除**了那条断言（纪律）

一条**无法失败**的断言会给**假保证**。按本轮之前一贯的纪律
（round 20 / 45：没有可失败探针的改动不留），我把它**删掉**，而不是留着好看。

Case 34 现在只保留 **3 条真正载荷**的断言（`watchCount() == 0` 系列）。
**并明确它们的界限**：它们断言的是**计数被清零**，而 `Reset` 是**分开**清表与清计数的，
所以它们**同样抓不住"保留条目"**。⇒ **"re-arm 清表"这条性质目前没有任何可失败守卫。**

## 6. 这是一个**需要裁定**的真实缺口（不是我能自行补的）

要让窗口维度可观测，至少有三条路，**每条都有代价**：

| 方案 | 代价 |
| --- | --- |
| **A** 把窗口段放进 `Entry` **并放上 wire**（需抬版本 v5） | 写方/读方/测试三方同步 + 版本表；裁定要求"新写方一律 v4" ⇒ **改版本需裁定** |
| **B** 加一个"条目窗口 ≠ 当前窗口"的**可见计数**（fail-visible） | 新计数器要进 counters 块 ⇒ 同样触版本表（round 10 的 `CLOSED_FIELDS` 陷阱） |
| **C** 只加**运行时断言**（debug 下崩溃/记录），不上 wire | 不触 wire；但生产是 release，**默认不生效** ⇒ 只能保护开发期 |

⇒ **我不替裁定选**。本轮的价值是把"真空满足"这个偏乐观的说法，
   换成"**违规也不可观测**"这个**准确**的说法，并说明它需要一条裁定。

## 7. 状态

```
pshot before SUMMARY: emitted=1 terminalEmitted=0 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=0 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 33 passed, 0 failed
wire=0
CHECKS=1160 FAILURES=0
site=F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
DLL=4A56E92B402DCFA9A28B30AC6DFCBA4B0B7CA9CF561C200842E10A4BDE7A2C3A
站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（与基线逐字相同 ⇒ 未部署）
git  = 无写操作
```

## 8. 不声称

- **不**声称窗口维度**已可观测**（恰恰相反）；
- **不**声称"保留条目"的情形在**实机**上出现过（它是一个**假设的**变异，不是观测）；
- **不**声称方案 A/B/C 中哪一条正确（**需裁定**）；
- **不**声称 C 阶段完成 —— 这一条按字面仍未完成，且现在**理由更硬**；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。