# 阶段 C：终态表改为**按版本校验**；两个读方一致登记 v4 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**只改读方（Python）**，
> **未改产品源码、未重建 DLL**（DLL 哈希与上一轮相同，已核对）。

## 1. 动机（裁定明确要求）

> 「终态表与阶段表必须**按版本校验**（**不得只把 7 加进全局 TERMINALS**）」

原状：`STAGES` 已按版本选表（`STAGES` / `LEGACY_STAGES`，`:417-419`），
但 **`TERMINALS` 是全局的** —— 一旦把 `7` 加进去，v1/v2/v3 产物就会开始**接受**
一个它们从未产生过的终态，读方丢掉「这个版本不该出现这个值」这一判据。

## 2. 改动

`AutoTest/analyze_palette_object_evidence.py`：

```python
TERMINALS_OBSERVATION_CLOSED=7
TERMINALS_V4=dict(TERMINALS);TERMINALS_V4[TERMINALS_OBSERVATION_CLOSED]='ObservationClosed'
TERMINALS_BY_VERSION={1:TERMINALS,2:TERMINALS,3:TERMINALS,4:TERMINALS_V4}
PALETTE_OBJECT_CHAIN_TYPED_VERSION=4
# 解码处按版本取表（未登记版本回退到基线表，仍由版本门控整份拒绝）
'terminal':lookup(TERMINALS_BY_VERSION.get(version,TERMINALS),bits[18],'palette terminal'),
```

`AutoTest/analyze_frame_evidence.py`：新增 `PALETTE_OBJECT_CHAIN_TYPED_VERSION=4`
并登记其块字段集（**目前与 v3 同集**；链型等字段会与写方**同一次**改动一起加入，
不得先登记后落空）。

**语义记档**：`ObservationClosed=7` = Observation 链「观察已结算」，
**不表示**阶段齐全、不表示身份已证明、不表示 palette 已消费、更不表示画面已恢复。

## 3. 未做（**明确**）

- **写方仍未输出 v4** —— `war3_frame_evidence.cpp:79` 仍是 `firstSightUsed() ? 3 : 2`；
  本轮**没有**动它（下一步）；
- `ObservationClosed` **尚未上 wire**（C++ 枚举未加、sink 未发）；
- 链型 `chainType` 仍**未参与查找**、未上 wire。

⇒ 本轮是**纯读方架构改动**：为 v4 打开通路，并让"加 7"必须落在**版本维**上。

## 4. 连带修正的三个**假失败**（同一模式：把"已登记版本"当"未知版本"）

| 文件 | 原样本 | 问题 | 处置 |
| --- | --- | --- | --- |
| `test_palette_object_evidence_analysis_static.py` | `(0, 4, 7, …)` | 4 已登记 | 改 5，**要求未变** |
| `test_analyze_frame_evidence.py` | `(0, 3, 7, …)` | **3 早已登记**（我只加了 4）⇒ **既存假失败**，不在 259 门禁内故未被发现 | 改 5，**要求未变** |
| `test_semantic_build_thread_gate_static.py` (`:1992`) | `(0, 4, 7, …)` | 4 已登记 | 改 5，并**新增 v4 正向见证**（与 v3 对称） |

**注**：第二条是**既存**缺陷（与我的改动无关）—— 失败列表不含 4，而 3 在我改动前就已登记。
它说明**门禁范围本身有缺口**：`test_analyze_frame_evidence.py` 不匹配 `test_*_static.py`，
因此长期不在 259 之内。

## 5. 状态

```
AutoTest 全量静态          : STATIC: 259 scripts, 0 failed
meson                     : Ok: 85  Fail: 0
ninja -C build32 -n       : no work to do
wire roundtrip            : CHECKS=1141 FAILURES=0
test_analyze_frame_evidence        : Ran 55 tests — OK
analysis static (palette object)   : Ran 100 tests — OK
候选 DLL                  : 36,297,472 B
                            SHA-256 1646FAE8BCDB9B2C35CB5145E7C6B26F55AE86EC83959E5E005D7DE32E306E50
                            （与上一轮**相同** ⇒ 本轮确为纯读方改动）
站点                      : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                       : 无写操作
```

## 6. 不声称

- **不**声称 v4 已实现（写方未发、终态 7 未上 wire、链型未参与查找）；
- **不**声称阶段 C 主体完成；wire 版本仍是 `firstSightUsed()` 动态 2/3；
- **不**声称 K2（未拒绝链被标 `Recovered`）、K3（混合链/跨帧语义）已修；
- **不**声称 259 个静态脚本之外不存在其它红点 —— 本轮恰恰发现了两个此前
  **不在门禁内**的失败，说明门禁范围有缺口（已记档，未扩大门禁）。
