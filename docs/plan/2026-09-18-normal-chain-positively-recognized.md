# 🎯 P1 修复：正常观察链首次被**正面认定**（实机验证）— 2026-09-18

> 对抗性审计指出：读方的完成判据硬编码 `len(stages)==4`，那是**拒绝恢复链**的形状；
> 而正常链按设计**永不发** ServedCandidate ⇒ 该字段对正常链**结构性不可达**。
> 本文记录修复，以及**在实机导出上的验证结果**。

## 1. 修复

```python
# 修复前
stage_observation_complete=(not truncation and not issues and len(stages)==4
                           and saw_submit and saw_draw and terminal!='NoTerminal')

# 修复后：按链型取要求数
required_stage_count = 3 if i_firstsight is not None else 4
stage_observation_complete=(not truncation and not issues
                           and len(stages)==required_stage_count
                           and saw_submit and saw_draw and terminal!='NoTerminal')
```

| 链型 | 完整阶段集 | 数量 |
| --- | --- | --- |
| 拒绝恢复链 | {Rejected, ServedCandidate, Enqueued, Drawn} | 4 |
| **正常观察链** | **{FirstSight, Enqueued, Drawn}** | **3** |

**这不是放宽**：两侧都是**完整**阶段集；原先的 4 对正常链要求一个**按设计不存在**的阶段。

## 2. 实机验证（导出 `cpu-31736-28653959291-1.json`，`DRAWS=0` 运行）

```
chains with FirstSight: 32
   stageObservationComplete: {'True': 32}          <-- 修复前恒为 False
   stages sets             : {"Drawn","Enqueued","FirstSight"}
   missingStages           : {}                     <-- 无缺失

chains without FirstSight: 75
   stageObservationComplete: {'False': 75}          <-- 仍然正确判否
   missingStages           : {'rejectedStageMissingFromStream': 75}
```

**这是正常观察链第一次被读方正面认定。** 而且：

| 性质 | 状态 |
| --- | --- |
| 32 条有链首的链 → `stageObservationComplete=True` | ✅ 达成 |
| 75 条缺链首的链 → 仍 `False` + 具名缺失 | ✅ **fail-visible 未被破坏** |
| `covered` | 仍为 False（`covered = not lossy and not truncation`；该导出的 `dropped*` 非零 ⇒ `lossy`）—— **这是正确的保守行为，不是失败** |

## 3. 验证

```
analyze 静态测试 : Ran 97 tests ... OK
全量静态         : 259 scripts, 0 failed
meson           : 85 Ok / 0 Fail
```

## 4. ③ 的进度

| 项 | 状态 |
| --- | --- |
| P0 stage=5 版本门控（v1/v2 整份拒绝）+ 行为测试 | ✅ |
| P1 deltaFrames 分支遗漏 + 多帧夹具 | ✅ |
| **P1 完成判据按链型（正常链可达）** | ✅ **本轮，含实机验证** |
| P2 夹具改真生产形状（`epochUnknown=true`+deviceEpoch=0） | ⏳ |
| P2 新计数器加行为校验（当前 `firstSightInserted=0, firstSightEmitted=1e9` 也被接受） | ⏳ |
| P2 场景 G 补齐 `check_header_block`/`check_decoded_events`/`check_chain` | ⏳ |
| — `NoteFirstSight` 每帧重发（"首次"名实不符）是否为本意 | ⏳ 待子线程 D |
| — 实机链**可认证**（`covered`）仍受 `dropped*` 非零限制 | ⏳ 见下方说明 |

## 5. 关于 `covered` 仍为 False（要说清）

`covered = not lossy and not truncation`。该导出 `droppedPerFrame/droppedPerSession` 非零 ⇒ `lossy=True`。
⇒ **"正常链被正面认定"不等于"实机链可认证"**。前者已经达成（本轮），后者要求丢失计数为零 ——
那需要进一步的预算/域配置工作（round 217 已把 `DRAWS=0` 固化为默认，可能已显著改善，但**尚未在干净条件下重测**）。

**我不把这两件事混为一谈。**

## 6. 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3；无 War3 进程；未提交、未部署、未晋升稳定。
```