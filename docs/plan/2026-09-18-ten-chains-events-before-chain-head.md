# 🎯 那 10 条 `stageObservationComplete=False` 已查明：**链首之前就有 Drawn 事件**（真实异常）— 2026-09-18

## 1. 现象

新导出 `cpu-22056-58874345715-1.json` 里，64 条含链首的链中 10 条 `stageObservationComplete=False`。

| | GOOD（54） | BAD（10） |
| --- | --- | --- |
| `stages` 集合 | `["Drawn","Enqueued","FirstSight"]` | **相同** |
| `terminal` | `WindowExpired` | **相同** |
| `missingStages` | `{}` | **相同**（无缺失） |
| `orderIssues` | `{}` | **`{drawnBeforeEnqueued: 10}`** |
| `sawDraw`/`sawSubmit` | True/True | 相同 |

⇒ 差别**只在顺序**上。

## 2. 实际阶段序列（这是关键证据）

```
BAD 链（10 条全部如此）：
  ['Drawn', 'Drawn', 'FirstSight', 'Enqueued', 'Drawn', 'Enqueued', ...]
  ['Drawn',        'FirstSight', 'Drawn',    'Enqueued', ...]
        ^^^^^^^^ 在链首之前！

GOOD 链（54 条）：
  ['FirstSight', 'Enqueued', 'Enqueued', 'Enqueued', 'Drawn', 'Drawn', ...]
        ^^^^^^^^^^ 链首在最前 ✅

统计：BAD = 10/10 的"首条 Drawn 早于首条 Enqueued"；GOOD = 0/54。
```

## 3. 为什么这是**真实异常**而不是解析器规则过严

`FirstSight` 的语义秩是 **0（链首）**，按设计它必须是整条链的**第一个**事件
（`PaletteObjectStage` 的注释与 `StageRank()` 都是这么定的）。

而 BAD 链里 **`Drawn` 出现在 `FirstSight` 之前** ⇒ **在"首次进入观察"被记录之前，该对象就已经有绘制记录**。

**阶段序列本身不自洽** —— 解析器判它 False 是**正确的**。

## 4. 根因推断：生产采集点看到的不是对象的整个生命周期

两个记录点：

| 阶段 | 记录点 | 触发条件 |
| --- | --- | --- |
| `FirstSight` / `Enqueued` | `d3d9_device.cpp:23528/23530` | `War3TryPopulateDrawTimeSemanticProducer` 的 `while (nextDrawTimeEntry(...))` 里，**每个实际提交的 draw-time caster** |
| `Drawn` | `d3d9_war3_shadow.cpp:5252` | 阴影接收 pass 记录到绘制命令 |

⇒ 若某对象**先**被阴影 pass 记录到绘制（`Drawn`），而在**之后的帧**才第一次走到生产采集点，
则 `Drawn` 必然早于 `FirstSight`。

## 5. 这个发现的含义（比"10 条链"更重要）

```
"FirstSight" 的真实语义 = "该对象第一次走到**生产采集点**"
                    ≠ "该对象第一次被观察/绘制"
```

那 10 条链证明：**存在对象在首次走到生产采集点之前就已经被绘制**。
因此：

| 影响 | 说明 |
| --- | --- |
| **命名与语义** | `FirstSight`（首次观察）这个名字**容易误导**；它实际是"生产采集点首见"。与命名相关的问题此前已记录过一次（每帧重发，已修） |
| **链首的唯一性假设** | 解析器把 `FirstSight` 当链首、要求它最先 —— 这个假设**在生产数据上会失败**（10/64） |
| **不能靠放宽解析器解决** | 放宽 `drawnBeforeEnqueued` 会让"链首之后才有事件"这个不变量失效，等于放弃顺序判据 |

## 6. 我**没有**做的判断（避免过度推断）

我**尚未**确定以下哪一项为真（需要更多取证）：

1. 生产采集点**本应**更早看到该对象，但存在一个**前置条件**（某个 `continue`/过滤器）把它漏掉了；
2. 或者这是**设计使然** —— 生产采集点只覆盖"draw-time semantic producer"这一子集，而阴影 pass 覆盖更广，
   两个域本来就不同源（这与此前记录过的"两条生产路径键口径不同、可能裂链"是同一类问题）。

**我不在证据不足时下结论**（这是本轮之前的教训）。

## 7. 下一轮的建议动作（具体、可执行）

```
observer 侧：检查 d3d9_device.cpp:23132 的 while 循环内是否有前置 continue/过滤
            （子线程 D 曾提到 :23386-23390 的 continue）
对照侧  ：找出这 10 个对象在 DRAWS=0 与 _DRAWS=1 两种配置下的差异
            （若关掉 Drawn 记录后 FirstSight 变成链首，说明是记录点时序问题）
决策    ：若属设计使然，则需在**读方**区分"链首"与"顺序"两类判据，
            或给 FirstSight 阶段正名（例如改称 ProductionFirstSeen），并同步三方
```

## 8. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（**已恢复基线**）✅  War3 进程: none
本轮未改任何文件（只做了只读分析，临时脚本已删除）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```