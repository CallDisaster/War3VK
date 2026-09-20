# 写方修正② 完成：预算预检提前到 Insert 之前 — 2026-09-18

## 1. 动机（独立复审 D 的建议）

`NoteFirstSight` 原先**先建条目、后判预算**。后果链：

```
预算拒发 ⇒ 条目已建（firstSightInserted++）却零阶段事件
⇒ 该条目只剩终态
⇒ 终态对 !sawFirstSight 的条目把 stage 回填为 Drawn（h:258-259）
⇒ 读方看到 [Drawn] 的**假链**（实机实测 **75 条**）
```

## 2. 改动（`war3_palette_object_evidence.h` 的 `NoteFirstSight`）

```
原先 :  Find → if(null) {Insert; inserted++} else if(sawFirstSight) {去重; return} → CanEmit → 发射
现在 :  Find → if(sawFirstSight) {去重; return} → CanEmit → if(null) {Insert; inserted++} → 发射
```

- **不变量**（新增）：表里有条目 ⇒ 链首一定发过；
- **代价**（明示）：预算紧张时该对象**完全不可见**（连终态也没有）—— 这是「不伪造事实」的方向；
- **丢失非静默**：`AccountDrop` 已计入 `droppedPerFrame`/`droppedPerSession`；
- **不改 wire**：不新增字段、不改版本，故三方读方无需同步。

## 3. 验证

```
war3_palette_object_evidence_test : SUMMARY: 24 passed, 0 failed
meson test                        : Ok: 85   Fail: 0
wire roundtrip                    : CHECKS=1137 FAILURES=0
静态全量                          : 259 scripts, 0 failed
ninja -C build32 -n               : no work to do
```

## 4. ⚠️ 未做的验证（不得越过）

- **实机 A/B 未做**：修正① 曾做实机 A/B（链首 32→64、重复发射 1304→64），**修正② 未做**；
- 因此**不能**声称「假链已从实机数据中消除」—— 只能说**单元/契约层面**已消除该形状；
- 实机验证需要一次完整周期（部署→采集→恢复），且预期的可观测差异是
  「75 条 [Drawn] 假链消失」+「N 个对象完全不可见（无链首无终态）」。

## 5. 现场

```
候选 DLL : 36,289,280 B   SHA-256 5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D
           （旧 77CAEED9… ⇒ 新 5ADEDD5F…；字节数相同，b_ndebug=false 下行号/调试信息偏移）
CAND_SHA : 已同步更新于 AutoTest/live_contrast_palette_objects.py
站点     : E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  未部署
```