# 锁住"链首之前有事件 ⇒ 不得认证"（实机取证后的回归锁）— 2026-09-18

## 1. 为什么需要这条锁

实机 `cpu-22056-58874345715-1.json` 里 64 条含首见的链中 **10 条**被判 `stageObservationComplete=False`。
查明后（见 `2026-09-18-ten-chains-events-before-chain-head.md` 与 `2026-09-18-producer-coverage-subset-of-drawn.md`）：

```
实际阶段序列： ['Drawn', 'FirstSight', 'Enqueued', 'Drawn', ...]
                       ^^^^^^^^ 链首之前就有 Drawn

根因：生产采集点只对"本帧序列号仍然当前"的 draw-time 缓存条目发 FirstSight/Enqueued
      （d3d9_device.cpp:23141-23142 的 frameSerial 过滤器），
      而 Drawn 由阴影 pass 记录 ⇒ 生产采集点的覆盖是阴影 pass 所绘对象的**真子集**。
```

⇒ **解析器判这 10 条 False 是正确的。**

**风险**：后来者看到"10 条链不完整"，很可能为了"让它们通过"而**放宽 `drawnBeforeEnqueued`**。
那会让"链内事件必须在链首之后"这个不变量失效 —— 等于放弃顺序判据，而顺序判据是这条链
"可认证"的基础之一。

⇒ 本条用例就是**阻止那种放宽**。

## 2. 用例

`test_palette_object_evidence_analysis_static.py` 新增：

```python
def test_event_before_the_chain_head_must_block_completion(self):
    # 顺序刻意做成 Drawn(4) 在前、Enqueued(3) 在后（且无 FirstSight）
    events_ = [event(1, key0, frames(render_frame=100), 4, window_segment=1, ..., chain_sequence=1,
                     saw_draw=True, saw_submit=True, delta_frames=0),
               event(2, key0, frames(render_frame=101), 3, window_segment=1, ..., chain_sequence=2,
                     saw_draw=True, saw_submit=True, delta_frames=0)]
    result = analyzer.analyze(envelope(events_, format_version=3))
    issues = [...orderIssues...]
    self.assertIn("drawnBeforeEnqueued", issues, ...)
    for c in result["chains"]:
        self.assertFalse(c.get("stageObservationComplete"),
                         "a chain with an event before its head must never be certified")
```

它断言两件事：

| # | 断言 | 防止的放宽 |
| --- | --- | --- |
| 1 | `drawnBeforeEnqueued` **必须**被检出 | 不许删掉/削弱这条顺序判据 |
| 2 | 该链**绝不**能 `stageObservationComplete=True` | 不许绕过顺序判据去认证 |

## 3. 验证

```
test_palette_object_evidence_analysis_static.py : Ran 98 tests ... OK（一次通过）
全量静态                                        : 见下
```

## 4. 我**没有**做的

- **未给 `FirstSight` 正名**。它的真实语义是「首次进入**这条生产路径**」而非「对象的首次观察」，
  正名（如 `ProductionFirstSeen`）需要**协调 write / 两个读方 / 测试 / 文档**一次改名，
  且 wire 编码值必须保持不变。这是一次**语义命名决策**，我不在自己单方面决定命名语义的范围外动手。
- 未验证 `War3DrawTimeExactRejectedCurrentFrame`（`d3d9_device.cpp:23143-23144`）在那 10 个对象上
  是否也参与了过滤。
- 未改动 `23141-23142` 的 `frameSerial` 条件（放宽它会退回"每帧发首见"，而配额 3584 **刚被 A/B
  证明是硬约束** —— 链首 32→64 靠的正是"每对象只发一次"）。

## 5. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮只改了 AutoTest 测试文件（新增一条守卫用例），未改产品源码。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```