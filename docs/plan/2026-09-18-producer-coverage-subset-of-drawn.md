# 🎯 定案：那 10 条链的机制 = 两个记录点**分母不同**（生产采集点只覆盖"本帧序列号当前"的条目）— 2026-09-18

## 1. 上一轮我提出的两个可能，现在有了**代码级答案**

上一轮我列出了 (a) 存在前置过滤器漏掉对象 / (b) 设计使然、域不同源，并**拒绝在证据不足时下结论**。
本轮读了生产采集点的循环，答案是 **(a) 与 (b) 的统一**：确实存在具体过滤器，而它造成的是**设计层面的覆盖差**。

## 2. 证据：`src/d3d9/d3d9_device.cpp:23132-23144`

```cpp
while (nextDrawTimeEntry(cacheKeyPtr, entryPtr)) {
  const auto& cacheKey = *cacheKeyPtr;
  auto& entry = *entryPtr;
  void* const renderablePart = cacheKey.renderablePart;
  if (renderablePart == nullptr)
    continue;                                                              // 23136-23137
  if (!entry.MatchesKey(cacheKey)) {
    continue;                                                              // 23138-23140
  }
  if (entry.frameSerial != m_war3ShadowPersistentFrameSerial)
    continue;                                                              // 23141-23142  ★
  if (War3DrawTimeExactRejectedCurrentFrame(cacheKey))
    continue;                                                              // 23143-23144  ★
  ...
```

## 3. 机制

| 记录点 | 覆盖什么 | 条件 |
| --- | --- | --- |
| `FirstSight` / `Enqueued`（`d3d9_device.cpp:23528/23530`） | 仅"**本帧序列号仍然当前**"的 draw-time VB 缓存条目 | `entry.frameSerial == m_war3ShadowPersistentFrameSerial` 且未被 exact-rejected |
| `Drawn`（`d3d9_war3_shadow.cpp:5252`） | 阴影接收 pass **实际渲染**的对象 | 无上述条件 |

⇒ **生产采集点覆盖的对象集合是阴影 pass 所绘对象的真子集**（还可能进一步被 `War3DrawTimeExactRejectedCurrentFrame` 缩小）。

## 4. 这如何产生"链首之前有 Drawn"

```
帧 N   : 对象先被阴影 pass 渲染        -> Drawn 被记录
         但它的 draw-time 缓存条目此时 frameSerial 不匹配 / 或尚未建立
         ⇒ FirstSight/Enqueued 本次不发
帧 N+k : 该条目终于成为"本帧序列号当前"  -> FirstSight 被记录（链首）
         ⇒ 链内顺序 =  Drawn(N) ... FirstSight(N+k) ...
```

⇒ 那 10 条链的 `['Drawn', 'FirstSight', 'Enqueued', ...]` 由此产生。

## 5. 定案结论（三条）

| # | 结论 |
| --- | --- |
| 1 | **不是解析器的错** —— 阶段序列确实不自洽（链首之前有事件），解析器判 `stageObservationComplete=False` 是**正确的** |
| 2 | **不是产品缺陷（在"两个域本来就不同"的意义上）** —— 生产采集点只承诺覆盖本帧序列号当前的 draw-time 条目，它**从未承诺**覆盖阴影 pass 的全部绘制 |
| 3 | **但它是一个真实的问题** —— 因为 `FirstSight` 的**命名与语义**声称"首次进入观察"，而实际是"**首次进入这条特定生产路径**"；解析器据此建立的"链首必须最先"不变量在生产数据上 **10/64 不成立** |

## 6. 我的建议（供决策，不由我单方面决定）

### 选项 A：**正名**（我倾向）

把 `FirstSight` 改名为能表达真实语义的名字，例如 `ProductionFirstSeen` / `ProducerAcceptedFirst`，
并同步：阶段枚举、`StageRank()`、wire 编码值（**值本身可不变**，只改名字与文档）、两个读方的 `STAGES` 表、
测试与文档。⇒ 消除"它是对象首次观察"的误解。

### 选项 B：让读方区分"链首"与"顺序"两类判据

读方不再要求"链首事件必须最先"，而是承认"链首 = 该链**被记录的第一条**恰好是生产首见"这一事实可能不成立。
**这会削弱顺序判据** —— 我不倾向，因为它把选项 A 要澄清的语义模糊当成正常。

### 选项 C：让写方把生产采集点扩到阴影 pass 的全部对象

移除/放宽 `23141-23142` 的 `frameSerial` 条件。**风险高**：那会让"每帧为所有被绘对象发首见"重新出现，
而**配额（3584）刚刚才被证明是硬约束**（本轮 A/B：链首覆盖 32→64 靠的正是"每对象只发一次"）。
⇒ 若同时放宽窗口与配额，会退回修复前的行为。**不建议。**

## 7. 我**没有**做的

- 未改任何代码或名字（这是**语义命名决策**，影响 write/read/test 三方，且选项 A 需要一次协调的改名）；
- 未验证 `War3DrawTimeExactRejectedCurrentFrame` 分支在那 10 个对象上是否也参与了（未取证）；

## 8. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```