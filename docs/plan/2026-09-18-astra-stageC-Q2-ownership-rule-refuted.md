# 阶段 C · Q2 ⑥：归属规则尝试**被实测推翻并回退** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**本轮没有留下前进性改动**，树已回到 round 19 的已验证状态。

## 1. 我尝试了什么

把 NoteServed / NoteEnqueued / NoteDrawn / NoteObjectGone 的 4 个查找点
从类型无关的 Find(key) 改为 FindOwner(key)，即启用 ⑥ 的归属规则：
**优先 Observation 链，其次 RejectionRecovery 链**。

## 2. 效果可观测（但**不是**正确的效果）

| | closedRecovered | closedWindowExpired | closedUnclosed |
| --- | --- | --- | --- |
| 改前（Find） | 1 | 1 | 0 |
| 改后（FindOwner） | 0 | 1 | 1 |

⇒ Enqueued/Drawn 被归入观察链 ⇒ 拒绝链只剩 Served ⇒ 不再构成 closedChain
（终态由 Recovered 变 Unclosed），观察链得 WindowExpired。

## 3. 为什么它是**错的** —— 一条真实的规则张力

我为该规则写的断言 Case 27 里失败：

```
FAIL: ownership: Enqueued/Drawn with both chains present must go to the OBSERVATION chain
```

**原因**：S/E/D 描述的是**同一条绘制流水线**。本用例里 Served 发生在观察链建立**之前**
（⇒ 归拒绝链），而 Enqueued/Drawn 发生在**之后**（⇒ 按优先级归观察链）。
于是**观察链没有 Served**，而 NoteEnqueued / NoteDrawn 在该链上缺少前序阶段 ⇒ 无法记录。

⇒ **「优先观察链」这条全局优先级规则是错的**。正确的方向应当是：
**每条事件跟随「已具备其前序阶段的那条链」**（即按流水线上下文归属），
而不是按链型的全局优先级。

**这是本轮真正的产出**：把"归属规则"从一个看似合理的直觉，推进为
「必须与阶段前序条件相容」这一**约束**。

## 4. 处置（同 round 4 / 8 / 16 的纪律）

- 4 个调用点**回退**为 Find(key)（已核对：FindOwner 调用点 4 → 0）；
- Case 27 里我加的归属断言块**移除**（它钉的是一条被推翻的规则）；
- **保留** FindOwner 的定义（**已定义、未被使用**），供下一轮按新方向重写；
- 构建通过、evidence 27/0、计数器回到 round 19 状态（closedRecovered=1 + closedWindowExpired=1）。

## 5. 状态

```
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
wire_exit=0
CHECKS=1160 FAILURES=0
DLL=4391429A59D115CF4A60345B5E034BE87E4E9DF1912A1E51358806FA81EE36FC
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 正确的下一步（归属规则重写）

```
① 先判定 NoteEnqueued / NoteDrawn / NoteObjectGone 各自的**前序阶段条件**
   （读实现：它们在缺少 sawServed / sawSubmit 等时是 return、计数丢弃、还是回退到别的条目）；
② 归属规则改为**按流水线上下文**：
   · 优先「已 sawServed（或该阶段前序）的链」；
   · 若两类链都满足/都不满足，再有确定的次序（并显式写进注释与测试）；
③ 断言必须**同时**覆盖：只有拒绝链 / 只有观察链 / 两类并存 三种形状；
④ 反向变异探针：把归属改回 Find(key) 时，新断言必须失败。
```

## 7. 不声称

- **不**声称 ⑥ 有进展（**回退**）；
- **不**声称归属规则已定义 —— 本轮只是**推翻了**一个错误候选并给出约束方向；
- **不**声称窗口维度已进入查找键；**不**声称读方会拒绝「链型与事件形状不符」；
- **不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。