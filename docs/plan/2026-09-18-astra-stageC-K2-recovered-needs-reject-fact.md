# 阶段 C：修 K2 —— `Recovered` 现在必须由**真实拒绝事实**支撑 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**改产品源码并重建 DLL**。
> 这是 round 8 清单里**不需要计数器级联**的那一项，因此可以独立、完整地落地。

## 1. 缺陷（K2）

`CloseWindow` 的 `closedChain`（`war3_palette_object_evidence.h`）**不含任何拒绝条件**：

```cpp
const bool closedChain = e.sawServed && e.sawSubmit && e.sawDraw &&
                         !e.orderViolation && !e.drawSelectionCleared &&
                         e.drawSource != PaletteObjectSource::None;
```

⇒ 一条**从未被拒绝过**的链，只要走到 S/E/D 就会被标成 **`Recovered`**。
而「恢复」在语义上**预设了「先失去」** —— 没有拒绝事实就没有"恢复"可言。

## 2. 修复

```cpp
const bool hasRejectFact = e.firstReason != PaletteObjectRejectReason::NotChecked;
const bool closedChain = hasRejectFact && e.sawServed && e.sawSubmit &&
                         e.sawDraw && !e.orderViolation &&
                         !e.drawSelectionCleared &&
                         e.drawSource != PaletteObjectSource::None;
```

**这不是收紧既有拒绝恢复链的行为**：由 `NoteReject` 建立的条目其 `firstReason` 恒为
真实拒绝原因，故它们的判定**一位不变**；而由 `NoteFirstSight` 建立的观察链条目
`firstReason = NotChecked`，因此**永不**可以使用 `Recovered`（裁定原文要求）。

## 3. 回归锁：`Case 26 Recovered requires a real reject fact (K2)`

两条**完全相同**的 S/E/D 链，唯一差别是条目由哪个入口建立：

| 分支 | 建立入口 | `firstReason` | 期望 |
| --- | --- | --- | --- |
| ① | `NoteFirstSight`（观察链） | `NotChecked` | **不得** `Recovered` |
| ② | `NoteReject`（拒绝恢复链） | 真实原因 `R1` | **仍必须** `Recovered` |

**② 是不可省的对照**：没有它，一个"把所有链都算成 `Unclosed`"的错误实现也会让①通过。

## 4. 载荷有效性（**实测，不是推理**）

临时把 `hasRejectFact &&` 从 `closedChain` 移除（即恢复未修状态）后重跑：

```
SUMMARY: 25 passed, 1 failed
FAIL: a chain with NO reject fact must never close as Recovered (K2)
```

还原修复后：`SUMMARY: 26 passed, 0 failed`。源码已逐字还原并核对无 `PROBE` 残留。

## 5. 状态

```
ninja -C build32 -n : no work to do
AutoTest 全量静态    : 259 scripts, 0 failed
meson               : Ok: 85  Fail: 0
evidence test       : SUMMARY: 26 passed, 0 failed   （原 25 + 新 Case 26）
wire roundtrip      : CHECKS=1153 FAILURES=0
候选 DLL            : 36,297,544 B
                      SHA-256 D156774C081A2149178ADC937C99954569236FEF0CADAF886A5FA383FFC7D5B1
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

## 6. 不声称

- **不**声称 `ObservationClosed` 已实现 —— 它**仍未上 wire**，因为它的分支顺序与
  计数器桶（`closedUnclosed` 会错标）必须**与读方 v4 counters 集一次做齐**；
- **不**声称 K3（混合链 `R→FirstSight` / 跨帧判序）已修；
- **不**声称链型已实现（`chainType` 仍未参与查找、未上 wire）；
- **不**声称阶段 C 主体完成；v4 目前 = v3 字段集 + 恒定版本号；
- `Recovered` 的**新约束**只堵住了"无拒绝事实却报恢复"这一条路径；
  它**不**证明被标 `Recovered` 的链在实机上真的恢复过（见全局边界："一条链结算不代表观察完整"）。
