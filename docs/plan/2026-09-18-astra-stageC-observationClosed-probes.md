# 阶段 C：补齐 `ObservationClosed` 的三条负向探针（并暴露两处版本无关用法）— 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**改产品源码 + 两个读方 + 夹具**。

## 1. 三条探针（round 10 记为未完成的三项）

| # | 探针 | 位置 | 内容 |
| --- | --- | --- | --- |
| **#1** | 终态 7 的**版本门控** | `test_palette_object_evidence_analysis_static.py` | v1/v2/v3 遇终态 7 必须拒绝**且理由必须是终态表**（`assertRaisesRegex(…, "palette terminal")`）；v4 必须**接受**同一形状 |
| **#2** | **桶标签** | `war3_palette_object_evidence_test.cpp` `Case 26` | `closedObservationClosed == 该终态的实际条数`，且 `closedUnclosed == 0`（不得错标） |
| **#3** | Observation 链的**直接**终态见证 | 同上 | 走完 S/E/D 的 Observation 链必须**恰好**是 `ObservationClosed`（此前只断言"不得是 Recovered"，一个"什么都判成 Unclosed"的实现也能通过） |

**#1 的两侧都钉住**：只拒绝不接受的实现、与只接受不拒绝的实现，都会失败。

## 2. 探针 #1 **当场暴露了两处版本无关用法**（这正是它的价值）

### 2.1 夹具侧

`envelope()` 辅助用**全局**表解码终态、并用 `TERMINAL_CLOSED_FIELD[terminal]` 分桶：

```
terminals.append(analyzer.TERMINALS[bits[18]])   -> KeyError: 7
```

**修正**：夹具改为**按版本**取表（`TERMINALS_BY_VERSION.get(format_version, …)`），
并**跳过该版本不认识的终态**（把"这不该出现"的判定留给被测对象 —— 否则夹具会先于
被测对象抛错，用例就失去特异性）。

### 2.2 分析器侧（**更严重**）

```
field=TERMINAL_CLOSED_FIELD[kind]                 -> KeyError: 'ObservationClosed'
'closedCounters':{name:counters[name] for name in CLOSED_FIELDS}  -> KeyError: 'closedObservationClosed'
```

⇒ 分析器有**两处**把"版本相关"的集合当成版本无关来用。round 10 我曾因此把
`CLOSED_FIELDS` 的登记**回退**掉；本轮改为**正确修法**：

- `TERMINAL_CLOSED_FIELD` 补上 `'ObservationClosed':'closedObservationClosed'`（映射必须是**全表**）；
- 所有 `counters[name] for name in CLOSED_FIELDS` 改为**按存在性过滤**
  （`if name in counters`，共 2 处）；
- 桶一致性检查里，若某桶不在该版本的计数器集内，记为**具名不符**而不是崩溃。

⇒ 这既保住了 v1/v2/v3 的解析，又让 v4 的桶一致性检查**真正生效**（round 10 的回退
是以"放弃检查"换取的，本轮不再如此）。

### 2.3 夹具载体的版本差异（第三个教训）

v1 把 `data[3]` 当**保留零**，v2+ 才启用分段 ⇒ 我第一版用例给所有版本都写了
`window_segment=1`，于是 v1 因**保留槽**先被拒，理由不是终态表。
**修正**：夹具按版本给合法载体（v1 → 0，v2/v3/v4 → 1）。这类"载体污染"在本项目已出现多次
（round 5 的经验：断言**拒绝理由**才能防止"因别的原因拒绝"冒充通过）。

## 3. 状态

```
STATIC: 259 scripts, 0 failed
DLL: 36297544 B  5931974B2C95E70FC320F627061EE72AE8F6A3001805D4E45D9DC9B27FE11A67
meson               : Ok: 85  Fail: 0
ninja -C build32 -n : no work to do
evidence test       : SUMMARY: 26 passed, 0 failed   （Case 26 现 6 checks）
wire roundtrip      : CHECKS=1160 FAILURES=0
读方套件            : analysis static 101 OK ; analyze_frame_evidence 55 OK
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

## 4. 不声称

- **不**声称终态 7 已在**实机**出现（D 批次完成前不新增实机因果结论）；
- **不**声称链型已实现：`chainType` 仍**未参与查找**、**未上 wire**；
- **不**声称 K3（混合链 `R→FirstSight`、跨帧判序）已修；
- **不**声称阶段 C 主体完成（Q2 的类型化查找键与事件归属规则**未做**）；
- 全局边界依旧：`ObservationClosed` **只**表示「观察已结算」。
