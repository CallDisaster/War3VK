# 阶段 C · 窗口维度：**今天不可观测** ⇒ 显式实现会不可验证（已回退）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**本轮尝试并回退**，产出是一个决定性结论。

## 1. 我尝试了什么

裁定要求查找维度扩为 **对象键 × 链型 × 窗口**。我按 round 19 的同构做法，
给 `FindChain` 的匹配谓词加上 `e.windowSegment == m_windowSegment`（并在不匹配时 continue），
并给 `Entry` 加 `windowSegment`、在 `Insert` 盖章。构建通过、31/0 全绿。

## 2. 然后核实"它是否可观测"——答案是**不可观测**

```
:224  Reset(session, epoch)                  { m_windowSegment = 1u;   清空**全部**条目; m_windowClosed = false; }
:246  ResetForSessionTransition(session, epoch){ ++m_windowSegment;      清空**全部**条目; m_windowClosed = false; }
:275  CloseWindow(frame)                     { ++m_windowSegment; m_windowClosed = true; }  // 不清条目，但**禁止**后续记录
```

⇒ **两条重新 arm 的路径都清空整张表** ⇒ 重新 arm 后**不存在陈旧条目**
⇒ 只按键+链型匹配与按键+链型+窗口匹配，**结果永远相同**。

全文件搜索：设置 `m_windowClosed = false` 的地方**只有** `:239` 与 `:261`，
且两者都在清表函数内 ⇒ **不存在"保留条目地重新 arm"的路径**。

## 3. 因此按纪律**回退**

我在 round 20 立过规矩：**没有可失败探针的改动不留**。
一个不可观测的谓词改动**无法被任何测试区分** ⇒ 留下它等于留一段
"看起来有意义、实际无人能验证"的代码。故已逐字回退（核对：无 `e.windowSegment` 残留、
`uint64_t windowSegment = 0u;` 出现 0 次），构建通过、31/0、no-work。

## 4. 但这也给出一个**实质结论**（本轮真正的产出）

**"查找维度含窗口"这一要求，在当前 API 下是*真空满足*的**：

| 层面 | 窗口维度现状 |
| --- | --- |
| **查找侧** | 由 `Reset`/`ResetForSessionTransition` 的清表**隐含保证** —— 每个窗口都从空表开始，因此不可能查到别的窗口的条目 |
| **导出侧** | **已实现且有测试**：`Entry/record.windowSegment` 写进 `data[3]`，读方 `chain_group_identity` **已把分段标签纳入分组键**（round 3 的裁定 ⑧），并有 `test_..._segment` 的按版本断言 |

⇒ 若要把查找侧的窗口分量**变成可验证的**，必须先引入一条
"**保留条目地重新 arm**"的路径 —— 那是一个**新能力**，不是本计划的修复项，
**需要裁定**。我**不替裁定新增这种能力**（那会改变 palette 观察的生命周期语义）。

## 5. 状态

```
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=560F42E4E0031ADA04E2CE5B6BEB220291F17044D41088A7A299D63308BC37D6
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称窗口维度"已完成" —— 我**回退**了显式实现；
  准确的说法是：**查找侧真空满足、导出侧已实现并测试**；
- **不**声称这个结论已由运行验证（它是**代码结构**的结论：两条 re-arm 路径都清表）；
- **不**声称"保留条目地重新 arm"是需要的（**未裁定**）；
- **不**声称 K3 生产侧已接线；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。