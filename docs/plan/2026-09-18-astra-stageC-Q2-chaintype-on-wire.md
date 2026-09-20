# 阶段 C · 步骤⑦（C++ 半边）：sink 写 `data[4] = chainType` — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**改产品源码并重建 DLL**。

## 1. 改动

`war3_palette_object_evidence_sink.cpp`：

```cpp
event.data[4] = static_cast<uint32_t>(record.chainType);
```

值来自 `MakeRecord` 从**条目建立时落定**的 `chainType` 盖章（round 12），**调用点不得给值**。
文件头注释同步登记该槽（v4 的**记录级**链型载体；版本 ≤3 读方仍要求它为保留零）。

## 2. 两处静态门禁的级联（**产品契约变更，非放宽**）

写入集断言原为 `{0,1,2,3,9,10,11}`，现为 `{0,1,2,3,4,9,10,11}`：

| 文件 | 处置 |
| --- | --- |
| `test_palette_object_evidence_analysis_static.py` | 登记集加入 4，**并加强**：断言 `data[4]` 必须由 `record.chainType` 写入（"登记了槽位"≠"槽位语义被钉住"） |
| `test_semantic_build_thread_gate_static.py` | 登记集加入 4（`11r(d)` 的未登记槽检查） |

## 3. 状态

```
=== static ===
STATIC: 259 scripts, 0 failed
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
exit=0
CHECKS=1160 FAILURES=0
=== DLL ===
36297544 B  E6B2CDC1857631E139C4D680FE26DFA17774D278D952C3012DAFE640671589E6
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 4. 现在 wire 上的链型是真的 —— 但仍**只**是标签

- 由 `NoteReject` 建立的条目 ⇒ `chainType = RejectionRecovery(0)`；
- 由 `NoteFirstSight` 建立的条目 ⇒ `chainType = Observation(1)`。

⚠️ **这不等于"两类链已独立"**：`Find`/`Insert` **仍未按链型分区** ⇒
一个对象**仍只能有一条条目**；`NoteFirstSight` 对已由拒绝建立的条目**仍会复用**
（`R→FirstSight` 改类缺陷**仍未修**），只是那条条目现在会带着它**建立时**的链型导出。

## 5. 不声称

- **不**声称两类链已独立、同一对象可同时持有两条（**⑤ 未做**）；
- **不**声称事件归属规则已定义（**⑥ 未做**）；
- **不**声称读方已按「对象键 × 链型」分组（**⑧ 未做**）；
- **不**声称 K3（跨帧判序）已修；**不**声称阶段 C 主体完成；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。
