# 阶段 C · Q2 ⑥：**撤回**上轮对归属规则失败的因果解释 — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读诊断，本轮无代码改动。**

## 1. 我上轮说了什么（现予撤回）

round 20 我记录：归属规则（优先 Observation）失败的原因是
「观察链没有 Served，而 NoteEnqueued / NoteDrawn 在该链上缺少前序阶段 ⇒ 无法记录」，
并据此把它写成「真实的规则张力」。

## 2. 直读代码后：该解释**不被支持**

NoteEnqueued（:557-582）的实际守卫只有两条：

```cpp
    Entry* e = Find(key);
    if (e == nullptr)
      return;                                   // 唯一的"条目缺失"守卫
    if (e->lastSubmitFrame == frames.renderFrame) {
      ... droppedDuplicatePerFrame / droppedPayloadConflict ...
      return;                                   // 同帧去重
    }
    if (!CanEmit(false)) { AccountDrop(false); return; }
    e->lastSubmitFrame = frames.renderFrame;
    ...
    e->sawSubmit = true;
```

全文件 `sawServed` / `sawSubmit` 的出现位置：

| 行 | 性质 |
| --- | --- |
| :132 / :765 | 字段声明 |
| :287 / :293 | **CloseWindow 的终态判定**（closedChain / WindowExpired） |
| :533 | NoteServed 里的**赋值** `e->sawServed = true` |
| :578 / :582 | NoteEnqueued 里的**赋值** |
| :309 / :490 / :537 / :628 / :655 / :801 | 拷进记录 |

⇒ **没有任何 `if (!e->sawServed) return;` 形式的前置守卫**。

**结论：我 round 20 的因果解释是错的（或至少未被证实）。**
那次断言失败的真实原因**仍未查明**，我不该把它当成已解释写入记录。

## 3. 因此当前的事实清单（严格只写已核对的）

- 把 4 个查找点改为 FindOwner 后，**计数器确实变了**：
  closedRecovered 1→0、closedUnclosed 0→1、closedWindowExpired 保持 1（这是**观测事实**）；
- 我写的断言 `enqOrDrawOnObservation == 2u && enqOrDrawOnReject == 0u` **失败**
  （这是**观测事实**）；
- 「观察链缺 Served 导致 E/D 无法记录」是**推断**，且与代码不符 ⇒ **撤回**。

**尚未排除的可能**（都需要实测，不再是推理）：

1. `NoteDrawn` 可能有**它自己**的守卫（我只读了 NoteEnqueued 的正文）；
2. 事件确实发了，但 `SameKeyFull` / 收集条件让它们没进 `g_collected`；
3. `CanEmit(false)` 在该帧被拒（会话/帧预算），于是事件根本没发；
4. `FindOwner` 在这两个调用点**没有生效**（我的脚本替换范围与实际调用点不符）。

## 4. 下一步（先把数打印出来，再谈规则）

```
① 在 Case 27 里**打印** enqOrDrawOnObservation / enqOrDrawOnReject / servedOnObservation
   / servedOnReject 的实际值 —— 先看数，不先解释；
② 直读 NoteDrawn（:602 起）与 NoteObjectGone（:645 起）的**全部**守卫；
③ 只有在数值与守卫都核对之后，才重新表述归属规则；
④ 反向变异探针：把归属改回 Find(key)，新断言必须失败。
```

## 5. 与项目纪律的关系

AGENTS.md 与既定裁定都要求：**一条链结算不代表观察完整**。
本轮是同一纪律在**我自己的推理**上的应用：
**一次计数器变化不代表因果已查明**。上轮我把「观测到的计数器变化」直接升格为「机制解释」，
这是**未经证实就下结论**，故本轮撤回并降级为待验证假设。

## 6. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 27/0 ; 读方 OK
候选 DLL : 4391429A59D115CF4A60345B5E034BE87E4E9DF1912A1E51358806FA81EE36FC
站点     : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git      : 无写操作
```

## 7. 不声称

- **不**声称归属规则失败的原因已查明（**已撤回**上轮的解释）；
- **不**声称 ⑥ 有进展；**不**声称窗口维度已进入查找键；
- **不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**。