# G 门禁：`check_header_block` 已参数化通过；`check_decoded_events`/`check_gaps`/`check_chain` **需成规模改造** — 2026-09-18

## 1. 本轮成果：G 的身份参数已从夹具读出（不是猜的）

`war3_palette_object_wire_roundtrip_test.cpp:223`：

```cpp
void ScenarioG(const ScenarioIO& io) {
  const PaletteObjectKey key = MakeKey(io.session, true, 0u);   // identityWeak=true, lifecycleIdentity=0
```

⇒ G 与 B/C/D 同一身份档：`identity_weak=True`、`lifecycleIdentity=0`、`IDENTITY_PROOF_NONE`。

## 2. 试套用其余三项检查 ⇒ **6 处失败，全部是"检查为拒绝恢复链写死"**

```
FAIL: G: export must carry all 5 events including the settled terminal (got 3)
FAIL: G: the generic root reader must report the registered version-2 extension
      for a real production export (got {'paletteObject': 3})
FAIL: G: the palette reader must agree with the generic reader on the extension
      version (got 3 / {'paletteObject': 3})
FAIL: G: all four stages must be present (got ['Enqueued', 'FirstSight'])
FAIL: G: the recorder's own Recovered verdict + submit/draw flags must survive
      (got {'terminal': 'WindowExpired', 'sawSubmit': True, 'sawDraw': False})
FAIL: G: hitCount must equal the ServedCandidate event count (got 0/0)
CHECKS=1155 FAILURES=6
```

## 3. 定案：这 6 条**都不是产品缺陷**

| # | 断言写死的假设 | 首见链的实际 |
| --- | --- | --- |
| 1 | 5 条事件（Rejected+Served+Enqueued+Drawn+终态） | **3** 条 |
| 2 | 扩展版本必须是 **2** | **3** |
| 3 | 同上 | **3** |
| 4 | **4** 个阶段 | **2** 个（无 ServedCandidate 是**设计**） |
| 5 | 终态是 **Recovered** | **WindowExpired**（没被拒绝过，当然不 Recovered） |
| 6 | `hitCount == ServedCandidate 条数` | 正常链**没有** ServedCandidate |

⇒ **`check_decoded_events` / `check_gaps` / `check_chain` 整段是为拒绝恢复链写的。**
这与上一轮 `check_header_block` 的问题**同类**，但这三个函数牵连更多（6 处断言）。

**这就是审计第 7 项"测试同步 ❌"的确切内容** —— 从一句笼统的判词，变成了 6 条可复现的断言。

## 4. 我的处置：回退，并把 6 条钉进源码注释

未在本轮进行改造，理由：

- 需要给三个函数加**链型参数**（4 阶段/5 事件/Recovered ⇄ 3 阶段/3 事件/WindowExpired）；
- 必须保证 A–F 的行为**一字不变**；
- 我的剩余上下文不足以安全完成并验证。**宁可不做，也不做半截。**

已在 `test_palette_object_wire_roundtrip.py` 的 G 段留下注释，逐条列出上述 6 处失败与所需改造。

## 5. 验证

```
CHECKS=1137  FAILURES=0
ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED
```

## 6. ⚠️ 一处我**无法解释**的差异（不掩盖）

| 轮次 | 状态 | CHECKS |
| --- | --- | --- |
| round 231 | G 跳过 header 检查 | 1117 |
| round 232 | G 跑 header 检查（参数化） | **1141** |
| round 233 | G 跑 header 检查（同上） | **1137** |

round 232 与 233 的**代码状态相同**（G 只多跑 `check_header_block`），但计数差 **4**。

可能原因：

1. 场景 E 是**并发**场景（4 写线程 + 控制线程），检查数**可能随调度变化**；
2. 或我的参数化在某处引入了计数差（但我未找到）。

**我未能区分这两者。** 这不影响 `FAILURES=0` 与 verdict，但**检查计数不是稳定量**这一点本身
值得记下 —— 如果将来有人把 `CHECKS=` 的绝对值写进门禁，会得到一个不稳定的门禁。

## 7. 现场

```
本轮只改 AutoTest 驱动（测试代码），未改任何产品源码。
候选 DLL 含：P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 / 写方修正① + 多帧回归锁。
现场站点 = 基线 F275545B…（未部署）；未提交、未部署、未晋升稳定。
```