# 阶段 C · 读方终态处理**系统审计**（否证 + 一处陈旧注释修正）— 2026-09-18（round 70）

> 动机：「版本无关用法」是本项目**已经撞过三次**的缺陷类
> （round 10/11 的 TERMINALS/CLOSED_FIELDS、round 28 的 FirstSight 规则）。
> 新增终态 ObservationClosed 之后，值得**系统**查一遍读方里所有触碰终态/结算的地方。

## 1. 审计方法与覆盖

在 AutoTest/analyze_palette_object_evidence.py 中枚举**全部** 97 处含 TERMINAL/terminal/closed/Closed
的位置，逐类核对：解码、表、桶、求和、结论逻辑、认证判据。

## 2. 结论：**未发现版本无关缺陷**（否证结果）

| 位置 | 内容 | 判定 |
| --- | --- | --- |
| :309 | TERMINALS={0..6}（旧表） | ✅ 保留原样 |
| :316-318 | TERMINALS_OBSERVATION_CLOSED=7；TERMINALS_V4；TERMINALS_BY_VERSION={1..3 同旧表, 4: V4} | ✅ **按版本** |
| :451 | 解码用 TERMINALS_BY_VERSION.get(version,TERMINALS) | ✅ 按版本 |
| :223 + :762 | OBSERVATION_CLOSED_FIELDS 只在 v4 的计数器集合里加入 | ✅ 按版本 |
| :267-268 | CLOSED_FIELDS 含 closedObservationClosed，且 :264-266 注释说明**不得**无版本区分地取值 | ✅ |
| :269-272 | TERMINAL_CLOSED_FIELD 含 ObservationClosed | ✅ |
| **:830** | closed_sum = 对 CLOSED_FIELDS **动态求和** | ✅ **无硬编码 6** |
| :908 | closedCounters 同样按 CLOSED_FIELDS 动态构造 | ✅ 同 |
| **:707-708** | else: conclusion = terminal | ✅ **新终态被通用处理**，不会被误标成 Unclosed |
| :692-693 | terminal != Recovered 时记 recoveryNotProven | ✅ 对观察链是正确语义 |

⇒ **本轮价值主要在"否证"**：新增终态**没有**在读方留下版本无关的窟窿。

## 3. 唯一真实问题：**陈旧注释**（已修）

:59 原文写「the **six** settled buckets sum to terminalEmitted」，而新增终态后是**七个**桶。
代码是动态求和（:830）所以**行为正确**，但**注释在撒谎** —— 而"注释里的计数"正是
round 65 那次 test_independent_review_sep18_fixes_static.py:55 失配的同类根源：
**读者会照着计数写出下一处版本无关断言**。

已改为：

    the seven settled buckets sum to terminalEmitted (the seventh, closedObservationClosed,
    exists only from v4 on -- see OBSERVATION_CLOSED_FIELDS -- and is never required for
    v1/v2/v3); and each closed* bucket equals the number of exported terminals of its
    own kind; watchCount <= kWatchCapacity(1024).

## 4. 观察到但**未改**的一处冗余（如实记录）

:830 有**重复条件**：for name in CLOSED_FIELDS if name in counters if name in counters。
行为无影响（幂等），我**未改** —— 按纪律，没有载荷测试的无害清理不值得动文件
（且它会让本轮 diff 变大、掩盖真正的修正）。

## 5. ⚠️ 本轮我改了三次才把一处注释改对（第 5 次"猜文本"类错误）

| 尝试 | 结果 |
| --- | --- |
| 1 | replace(six→seven) 并给下一行附 C++ 注释符 ⇒ **注释断句、且把 // 混进 Python 文件** |
| 2 | 删 2 行改 3 行 ⇒ **把续行内容删掉了**（number of exported terminals … 丢失） |
| 3 | 按残句定位并恢复续行 ⇒ ✅ 通顺 |

**规律（第 5 次同类）**：round 16 块边界 / 34 锚点 / 51 子串 / 52 猜 include 字面 /
68 锚点带引号致 findIndex 返回 -1 / **本轮删行未保留被删内容**。
⇒ **散文类改动的正确做法**：把**将被替换的整段原文**先读出来、整段替换，
而不是"按行数拼接"。

## 6. 状态

  读方 test_palette_object_evidence_analysis_static.py = OK
  读方 test_analyze_frame_evidence.py                 = OK
  站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
  git  = 无写操作

## 7. 不声称

- **不**声称审计**穷尽**了读方所有路径（枚举了含关键词的 97 处并逐类核对；关键词之外
  可能仍有相关逻辑，但**没有**迹象）；
- **不**声称本轮有任何**产品**改动（只改了读方里的一处**注释**）；
- **不**声称修注释有"行为影响"（它是**零行为影响**的文档修正）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。