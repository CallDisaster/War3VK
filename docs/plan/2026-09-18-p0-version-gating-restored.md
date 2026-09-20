# P0 修复：恢复 v1/v2 的阶段门控（对抗性审计后收紧）— 2026-09-18

> 对抗性审计指出 ③ 的 7 项约束里 3 项不成立。本文记录其中**最严重的两条 P0 的修复**。
> **两条都是"收紧"，不是"放宽"** —— 目的正是恢复复审明令的约束。

## P0-1：让 stage=5 的接受**按版本门控**

### 缺陷

`analyze_palette_object_evidence.py:287` 的注释**已经写明应有行为**：

```python
# FirstSight 故意取 5：版本 1/2 的形状里它不可解释，读方必须**拒绝**而不是静默忽略。
```

但第 289 行的 `STAGES` 是**版本无关**的，且解码处 `:413` 直接用 `lookup(STAGES,...)`：

```python
'stage':lookup(STAGES,bits[5],'palette stage'),   # 不看 version
```

⇒ v1/v2 载荷里出现 stage=5 时，读方**认识**这个值 ⇒ 只加软标记、**不拒绝整份导出**。
而改动前阶段表里没有 5 ⇒ `require(value in table)` 会**整份拒绝**。
⇒ **我把 v1/v2 的既有语义放宽了**，这正是复审"保留既有拒绝恢复链"所禁止的。

### 修复

```python
# 版本门控：v1/v2 只认识 1..4
LEGACY_STAGES={1:'Rejected',2:'ServedCandidate',3:'Enqueued',4:'Drawn'}

'stage':lookup(
    STAGES if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION else LEGACY_STAGES,
    bits[5],'palette stage'),
```

⇒ v1/v2 遇到 stage=5 时，`lookup` 的 `require` 抛 `ValueError: unknown palette stage 5`，
**整份导出被拒绝** —— 恢复改动前的 fail-visible 行为。

## P0-2：`:485` 的豁免显式加版本条件（纵深防御）

原代码：

```python
if i_reject is None and i_firstsight is None:
    truncation.append('rejectedStageMissingFromStream')
```

它**无条件**豁免"含 FirstSight 的任何链"，未要求 `version>=3`。
P0-1 生效后 legacy 版本已不可能走到这里（会在解码处被拒），但我仍补上显式条件：

```python
if i_firstsight is not None and version<PALETTE_OBJECT_FIRST_SIGHT_VERSION:
    raise ValueError('firstSight stage under legacy version %d' % version)
```

理由：防止将来有人放宽解码后，这条豁免又变成跨版本的漏洞。

## 验证

```
analyze 静态测试  : Ran 95 tests ... OK
全量静态脚本       : 259 scripts, 0 failed
meson test        : 85 Ok / 0 Fail
```

## 我**没有**做到的（必须写清）

审计指出这些新判据"只有字符串锚点、**零行为用例**"。我**这一轮只改了实现，没有补行为测试**。
所以现在的状态是：**行为已收紧，但还没有一条用例真的构造 v1/v2 + stage=5 去断言它被拒**。
现有 95 条解析器测试全绿**不能**作为这条修复已被覆盖的证据 —— 它们本来就没覆盖它。
⇒ 补行为测试列入 P1。

## 剩余项（审计给出，按优先级）

| 优先级 | 项 | 状态 |
| --- | --- | --- |
| P0 | stage=5 按版本门控 | ✅ 本轮修复 |
| P0 | `:485` 豁免加版本条件 | ✅ 本轮修复 |
| P1 | deltaFrames 写方/读方契约冲突（探针 P2 已复现 `frozenZeroDeltaViolated`；**我的夹具只跑单帧所以看不到**）+ 加多帧夹具 | ⏳ |
| P1 | v3 完成判据适配正常链（`len(stages)==4` 要求 ServedCandidate ⇒ 结构上不可达） | ⏳ |
| P1 | 补 P0 的**行为**测试 | ⏳ |
| P2 | 夹具改成真生产形状（`epochUnknown=true` + deviceEpoch=0） | ⏳ |
| P2 | 新计数器加行为校验（当前 `firstSightInserted=0, firstSightEmitted=10^9` 也被接受） | ⏳ |
| P2 | 场景 G 补齐 `check_header_block`/`check_decoded_events`/`check_chain`（当前跳过） | ⏳ |
| — | `NoteFirstSight` 每帧重发（"首次"名实不符）—— 需先确定正确语义再动 | ⏳ |

## 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3（未改动）；无 War3 进程；未提交、未部署、未晋升稳定。
```