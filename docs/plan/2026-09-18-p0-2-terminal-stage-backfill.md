# P0-2 终态阶段回填 — 2026-09-18（未提交 / 未部署 / 未晋升稳定）

## 1. 缺陷（Astra 实测，我独立复核并**精确复现**）

`MarkStage()`（`war3_palette_object_evidence.h`）有三条分支，而 `maxStage` 只被其中一条更新：

```
attemptSerial == ~0ull（未知） ⇒ 更新 maxStage        ✅
attemptSerial != e->attemptSerial（新尝试） ⇒ 只更新 attemptStage + attemptSerial   ❌ maxStage 不动
同一尝试 ⇒ 只更新 attemptStage                        ❌ maxStage 不动
```

而 `CloseWindow` 的终态回填用 `maxStage`：
`record.stage = e.sawFirstSight ? StageOfRank(e.maxStage) : Drawn;`

⇒ 结果：
· 五点携带**同一个已知**尝试号（Astra 的反例）⇒ 首点走「新尝试」、其后走「同尝试」⇒ **两条分支都不提升 maxStage**
  ⇒ 已走到 Drawn 的链被回填成 **FirstSight**（Astra 实测 `terminal=ObservationClosed stage=FirstSight sawDraw=true`）
· 五点携带**不同**已知尝试号（生产上就是各点的帧号）⇒ 每点都走「新尝试」⇒ 同样永不提升

注：`PaletteObjectStage{Rejected=1, ServedCandidate=2, Enqueued=3, Drawn=4, FirstSight=5}`，
所以 Astra 报的 `stage=FirstSight` 对应数值 **5**。我的探针实测到的正是 `terminalStage=5`。

## 2. 修复（一处，语义分离）

在 `MarkStage` 开头、与尝试号**无关**地维护「终身最高秩」：

```cpp
const uint32_t s = StageRank(stage);
if (s > e->maxStage)   // ← 新增：终态回填用的事实，与关联范围无关
  e->maxStage = s;
```

字段职责现在是明确的：
```
maxStage     = 该条目**实际达到的最高语义秩**（终态阶段回填用）——与尝试关联无关
attemptStage = **本次尝试**的最高秩（只用于同尝试内的回退判定）
```

这与 Astra 的要求一致：**让回填反映实际事实**，而不是掩盖关联缺失。
「尝试号=帧号」造成的判序失效属于 **P0-5**，必须在那里显式解决，不能用本修复冒充。

## 3. 验证（全部检查退出码）

```
ninja -C build32 -j4     exit 0
ninja -C build32 -n      no work to do
AutoTest 静态全量          263 / 0
ninja -C build32 test    Ok 85 / Fail 0
宿主机测试                **36 passed, 0 failed**（新增 Case 37）
wire 驱动                CHECKS=1160 FAILURES=0
```

Case 37 实测：`[P0-2] sameKnownSerial terminalStage=4 (Drawn=4) stored=5` ⇒ `[PASS] 37 … (2 checks, 0 failures)`
Case 35 输出未变（`dual rec[6] stage=4 terminal=7`）⇒ 未知尝试号路径未被扰动。

## 4. 可失败性探针（**精确复现 Astra 的反例**）

```
撤掉 `if (s > e->maxStage) e->maxStage = s;` ⇒
  [P0-2] sameKnownSerial terminalStage=5 (Drawn=4)   ← **5 = FirstSight**，与 Astra 实测一致
  FAIL: P0-2: the terminal stage must reflect the highest rank ACTUALLY reached (Drawn) …
  [FAIL] 37 terminal stage backfill (P0-2) (2 checks, 1 failures)
```
已还原（逐字节相等校验通过）并复跑全绿。

## 5. 不声称

```
· 不声称判序（attempt 关联）已修 —— 那是 P0-5，本修复只修**回填事实**
· 不声称实机已验证（本轮零实机采集）
· 不声称 Astra 的全部 v4 契约问题已解决（ObjectGone / ObservationClosed / D 点 / wire 载体仍未做）
```
