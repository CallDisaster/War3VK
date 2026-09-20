# 阶段 C：修掉我在 round 256 引入的**误拒不变量**（K1）— 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**只改读方（Python）**，
> **未改任何产品源码、未重建 DLL**（DLL 哈希与上一轮相同，已核对）。

## 1. 缺陷（K1，Astra 指出；**我自己引入的**）

`AutoTest/analyze_palette_object_evidence.py:710`：

```python
first_sight_events=sum(1 for event in events if event['stage']=='FirstSight')
...
require(first_sight_events<=emitted,
        'export carries %d FirstSight events but firstSightEmitted=%d;'
        ' retention can never exceed emission'%(first_sight_events,emitted))
```

**终态回填**会把终态的 `stage` 写成该条目达到过的最高阶段。一条**只走到链首就关窗**的
**合法**观察链，会同时携带：

- 现场链首：`stage=FirstSight, terminal=NoTerminal`；
- 关窗终态：`stage=FirstSight, terminal=ObservationClosed/WindowExpired`（**回填**）。

⇒ `first_sight_events=2` 而 `firstSightEmitted=1` ⇒ 触发我加的
`retention can never exceed emission`，**整份导出被拒绝**。

**这是我在 2026-09-18 为关闭审计探针 B 而引入的缺陷。** 我当时的 98 个测试**没有覆盖
`FirstSight→CloseWindow` 这个形状**，所以没抓到 —— 这正是"检查存在"不等于"检查正确"。

## 2. 修复

```python
# ① 现场保留量 ≤ 发射次数（**排除终态回填**）—— 现在这才是正确的单向不变量
live_first_sight       = 计数 stage=='FirstSight' 且 terminal=='NoTerminal'
# ② 回填的阶段摘要同样不可能多于发射次数（更弱的检查，但能杀死伪造）
backfilled_first_sight = 计数 stage=='FirstSight' 且 terminal!='NoTerminal'

require(live_first_sight       <= emitted, '… retention can never exceed emission …')
require(backfilled_first_sight <= emitted, '… a chain cannot summarise a stage it never emitted …')
```

**注意这不是"放宽"**：① 的判据改为正确的量；② **新增**了一条此前不存在的检查
（回填不得多于发射）。净效果是判据**更准**而非更松。

## 3. 两个新用例（此前**零覆盖**）

`AutoTest/test_palette_object_evidence_analysis_static.py`：

| 用例 | 内容 | 作用 |
| --- | --- | --- |
| `test_first_sight_terminal_backfill_is_not_double_counted` | 现场 FirstSight + 回填 FirstSight 终态，`inserted=1/emitted=1` | **正面见证**：修复前必然失败 |
| `test_first_sight_terminal_backfill_without_emission_is_refused` | 只有回填 FirstSight 终态，`emitted=0` | **反向对照**：必须拒绝，且断言失败消息含 `backfilled` |

反向对照还**断言了拒绝理由**（`self.assertIn("backfilled", …)`），
因此"因为别的原因抛异常"不会伪装成通过 —— 这是我上一轮从"测试必须真的会失败"里学到的。

## 4. 载荷有效性（**实测，不是推理**）

把 ① 临时改回旧写法（`sum(… stage=='FirstSight')`，即计所有终态），只跑这两个用例：

```
Ran 2 tests in 0.003s
FAILED (failures=1, errors=1)
AssertionError: 'backfilled' not found in
  'export carries 1 live FirstSight events but firstSightEmitted=0; retention can never exceed emission'
```

⇒ 正面见证在旧不变量下**确实失败**（合法导出被拒），反向对照也因**理由错误**而失败。
还原修复版后：`Ran 2 tests — OK`。

**完整套件**：`Ran 100 tests — OK`（原 98 + 新 2）。

## 5. 本轮状态（全绿；**DLL 未变**）

```
AutoTest 全量静态    : 259 scripts, 0 failed
meson               : Ok: 85  Fail: 0
ninja -C build32 -n : no work to do
wire roundtrip      : CHECKS=1141 FAILURES=0
分析静态（本文件相关）: Ran 100 tests — OK
候选 DLL            : 36,297,472 B  1646FAE8BCDB9B2C35CB5145E7C6B26F55AE86EC83959E5E005D7DE32E306E50
                      （与上一轮**相同** ⇒ 本轮确为纯读方改动）
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

## 6. 不声称

- **不**声称终态回填的**语义**已定案 —— 本轮只修了"读方不要把它当成一次发射"；
  终态携带阶段摘要这件事本身是否该改（例如终态另用字段承载摘要），属**未决**；
- **不**声称 K2（未拒绝链被标 `Recovered`）、K3（混合链/跨帧语义）已修；
- **不**声称链型已实现（`chainType` 仍未参与查找、未上 wire）；
- **不**声称阶段 C 主体完成；wire 版本仍是 `firstSightUsed()` 动态 2/3；
- 本轮**未**构造"实机导出"级别的端到端见证 —— 见证是**合成夹具**层面的。
