# P2 完成：新版首见计数器加**行为校验**（补上审计探针 B 的漏洞）— 2026-09-18

## 1. 审计指出的漏洞

> "读方只要求 v3 块**存在**两个首见计数器（`:662-666`），从不与事件或彼此比对。
> **探针 B**：`firstSightInserted=0, firstSightEmitted=10**9` 被接受，`coverageComplete=true`。"

## 2. 加的两条**单向不变量**

`AutoTest/analyze_palette_object_evidence.py`（紧跟 `events` 解码之后，仅在 `version>=3` 时生效）：

```python
if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION:
    first_sight_events=sum(1 for event in events if event['stage']=='FirstSight')
    inserted=counters['firstSightInserted']
    emitted=counters['firstSightEmitted']
    # ① 有发射必然有插入：一条链首事件必然对应一次「建条目」。
    require(emitted==0 or inserted>=1,
            'firstSightEmitted=%d while firstSightInserted=0: an emission requires an entry' %emitted)
    # ② 导出里保留的 FirstSight 事件数不可能多于发射次数（丢失只会减少保留量）。
    require(first_sight_events<=emitted,
            'export carries %d FirstSight events but firstSightEmitted=%d;'
            ' retention can never exceed emission'%(first_sight_events,emitted))
```

**为什么是单向的、且不是"迁就实现"**：

| 不变量 | 为什么对**修改前与修改后**的写方都成立 |
| --- | --- |
| ① `emitted>0 ⇒ inserted>=1` | 任何发射都发生在某个条目上（修改前也是先 `Find`/`Insert` 再发射） |
| ② `导出中的 FirstSight 事件数 <= emitted` | 丢失（子门短路 / 编码失败 / 环淘汰）只会**减少**保留量，绝不会增加 |

⇒ 两条都**只**排除"不可能的形状"，不要求任何特定实现细节。

## 3. 立即可测：审计探针 B 现在会被拒

```
firstSightInserted=0, firstSightEmitted=10**9
  -> 不变量① 触发：emitted=1000000000 > 0 而 inserted=0  ==> REJECT
firstSightInserted=5, firstSightEmitted=1, 导出含 3 条 FirstSight
  -> 不变量② 触发：3 > 1  ==> REJECT
```

## 4. ⚠️ 加上不变量后，**我自己的两个夹具被拒了**

```
ERROR: test_first_sight_chain_anchor_delta_spans_multiple_frames
  ValueError: export carries 2 FirstSight events but firstSightEmitted=0; retention can never exceed emission
ERROR: test_first_sight_stage_is_rejected_wholesale_under_legacy_version
  ValueError: export carries 1 FirstSight events but firstSightEmitted=0; retention can never exceed emission
```

⇒ **两个都是我此前写的用例**（round 221 / round 218）：它们构造了 FirstSight 事件却**没有声明**对应的发射计数。

**处置：修夹具，不放宽不变量。** 用 `envelope(..., counter_overrides=...)` 补上应有的计数：

```python
# 用例1（2 条 FirstSight 事件）：1 个条目、发射 2 次 —— 正是**修改前写方**的形状
counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 2}
# 用例2（1 条 FirstSight 事件）
counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1}
```

**注意**：不能用 `counter_overrides` 给 legacy 版本加字段 —— `expected_counters` 对 v<3 不含这两项，
多传会导致"计数字段不匹配"。故只在 v3 的调用点上传。

## 5. 验证

```
analyze 静态测试     : Ran 98 tests ... OK
全量静态             : 259 scripts, 0 failed
四份实机导出仍被接受   : cpu-10976 / cpu-5204 / cpu-22056 / cpu-31736   均 OK, chains=107
```

⇒ **新不变量不拒绝任何真实数据**，同时补上了漏洞。

## 6. 至此的 ③ P2 进度

| P2 项 | 状态 |
| --- | --- |
| 新计数器加**行为**校验 | ✅ **本轮完成** |
| 夹具改真生产形状（`epochUnknown=true`+deviceEpoch=0） | ⏳ 会牵动 A–G 大量断言 |
| 场景 G 补齐被跳过的三项检查（`check_decoded_events`/`check_gaps`/`check_chain`） | ⏳ 需链型参数化，已有 6 处失败清单 |

## 7. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮只改了 AutoTest 解析器与测试文件，未改产品源码。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```