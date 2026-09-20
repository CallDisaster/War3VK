# P1 修复：deltaFrames 分支遗漏（读方）+ 多帧夹具 — 2026-09-18

## 定位过程

上一轮我把 deltaFrames 冲突描述为"写方/读方的契约分歧"。**进一步读代码后发现更准确的定性**：

`analyze_palette_object_evidence.py:564-571`：

```python
if event['stage'] in ('Rejected','ServedCandidate') or event['terminal']!='NoTerminal':
    if frame>=first_reject and delta!=frame-first_reject:
        issues.append('deltaFramesMismatch@%d'%event['sequence'])
elif delta!=0:
    issues.append('frozenZeroDeltaViolated@%d'%event['sequence'])
```

⇒ 读方**确实有**"delta == 本帧 − 锚点"这条规则，但那个分支**只认 Rejected/ServedCandidate/终态**，
**漏了 `FirstSight`**。于是首见链的 FirstSight 落入 `elif`，被要求 `delta==0`。

而写方注释（`h:403-407`）明写契约是 `delta = 本帧 − **链首帧**`。

⇒ **这是分支遗漏**（读方少写了一个阶段名），而不是"两侧对字段含义有不同主张"。
我上一轮的描述偏重了，这里更正。

## 修复

```python
if (event['stage'] in ('Rejected','ServedCandidate','FirstSight')
        or event['terminal']!='NoTerminal'):
```

**注意这不是放宽**：该分支内的校验仍是严格的 `delta == frame − first_reject` 等式；
只是把首见链的**锚点事件**纳入其中。原先落入 `elif` 时那条 `delta==0` 的要求，
对首见链的首次事件与后续事件**本就无法同时成立**（首次为 0、后续 >0）。

## 为什么此前没有用例抓到（审计的关键洞察）

往返夹具 G **只跑单帧** ⇒ `renderFrame == 链首帧` ⇒ `delta == 0` ⇒ **恰好满足 `elif` 的要求**。
⇒ **用例的形状恰好掩盖了缺陷。** 只有"两帧同一对象"才暴露。

## 新增多帧行为夹具

`test_first_sight_chain_anchor_delta_spans_multiple_frames`：

```python
# 首见链的锚点帧就是首见帧：两条事件的 firstRejectFrame 都写 100（承载"链首帧"）
seq = [event(1, key0, frames(render_frame=100), 5, ..., first_reject_frame=100, delta_frames=0),
       event(2, key0, frames(render_frame=103), 5, ..., first_reject_frame=100, delta_frames=3)]
result = analyzer.analyze(envelope(seq, format_version=3))
assert 'frozenZeroDeltaViolated@2' not in issues      # 修复前会命中
assert not [i for i in issues if 'deltaFrames' in i]  # 锚点等式必须成立
```

**过程中我自己写错过一次夹具**：`first_reject_frame` 用了默认 0 而帧是 100/103 ⇒
报 `deltaFramesMismatch@1/@2`；改为两条都写 `first_reject_frame=100`、`chain_sequence=1/2` 后通过。

## 验证

```
analyze 静态测试 : Ran 97 tests ... OK
全量静态         : 259 scripts, 0 failed
meson           : 85 Ok / 0 Fail
```

## 仍未决

`NoteFirstSight` **每帧重发**是否为本意：

| 若"应只发一次" | 则 delta 冲突自然消失、命名 ("首次") 与 `orderViolation` 问题一并解决 |
| 若"每帧重发是本意" | 则本轮的分支修正就是最终解 |

子线程 D 正在查证。**本轮未改写方** —— 不在语义未定论时改。

## 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3；无 War3 进程；未提交、未部署、未晋升稳定。
```