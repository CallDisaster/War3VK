# R3 wire 形状回归已修复：全量静态与 meson **双双全绿** — 2026-09-18

> 承接 `...-r3-wire-shape-regression.md`。本文记录修复过程、选择的方案与最终验证。

## 1. 消费者调查（决定方案）

| 字段 | AutoTest 引用 | src 引用 | 结论 |
| --- | --- | --- | --- |
| `enqueueBlockReached` / `productionInsertReached` / `productionNoteCalled` / `appendEntered` / `resetOrClearCount` | **0** | 1（仅写入点） | 无消费者 |
| `activeSession` | 1 —— 但实为 `"activeSessions"`（复数，**另一个**字段） | 1 | 无消费者 |

⇒ **方案 X 安全**：按 R3 的原始验收要求，只保留 sink 内部的原子计数与 gating，不把它们暴露到线上 wire。

## 2. 两处修复

### 2.1 块头字段（方案 X）

`war3_frame_evidence.cpp`：删除此前追加的 8 个块头字段，并**写明不得再加**：

```
armed / activeSession / enqueueBlockReached / appendEntered /
productionInsertReached / productionNoteCalled / resetOrClearCount
```

注释中记录：R3 的三条验收性质（原子化、位于子门判断之后、关闭后不再更新）由 sink 内部的
`std::atomic` 与调用点 gating 保证，**不依赖**暴露到线上 wire；若将来确需这些量，**必须抬版本**，
不得在既有版本块追加字段，也不得放宽读方注册表。

### 2.2 counters 首见键（同类问题的第二处）

修完块头后仍有 `paletteObject counter fields mismatch`（A/B/C/D 四场景）。根因同型：
批次 3 把 `firstSightInserted` / `firstSightEmitted` **无条件**加进了 counters 映射，
而版本 2 的 counters 是**注册集合**。

修复：把这两个键**只在版本 3**（`firstSightUsed()`）时写入：

```cpp
if (PaletteObjectRecorder().firstSightUsed()) {
  counters["firstSightInserted"]=std::to_string(c.firstSightInserted);
  counters["firstSightEmitted"]=std::to_string(c.firstSightEmitted);
}
```

## 3. 最终验证（全量重建后）

```
ninja -C build32            -> 完成
ninja -C build32 -n         -> no work
全量静态脚本                 -> 258 scripts, 0 failed
meson test                  -> 85 Ok / 0 Fail
war3_skin_palette_admission_test   -> 6 passed, 0 failed
war3_palette_object_evidence_test  -> 24 passed, 0 failed
war3_palette_object_evidence_cost  -> all checks passed
war3_frame_evidence_runtime_test   -> checks=723 accepted=10000 PASS
```

⇒ **本会话首次**全量静态与 meson **同时全绿**。

## 4. 修复过程的中间态（诚实记录）

| 步骤 | 结果 |
| --- | --- |
| 只删块头 8 字段 | 形状/根读取器失败消失，但出现 `counter fields mismatch` |
| 再 gate counters 首见键 | meson **85/0** |

两处是**同一类错误的两个实例**：在未抬版本的协议块里追加字段（一处加在块头、一处加在 counters）。

## 5. 边界（不得含糊）

- 已跑：**全量静态 258/258**、**全量 meson 85/85**、4 个关键 runnable。
- **未跑**：全量 Win32 runnable 套件（约 15-24 个）、TDR/ABBA、玩家前台门。
- **未做**：实机运行；批次 4 第 ④ 问中"**正常对象是否被误伤**"仍只有**读码 + 纯谓词**证据，**无运行期证据**。
- 未提交、未部署；现场 DLL 仍为 `F275545B…`（`519AFA69…` 备份在位）；未改用户视频设置。

## 6. 精确表述

> 全量静态 **258/258 通过**；全量 meson **85/85 通过**；关键 runnable 4 项通过。
> **未**运行全量 Win32 runnable、TDR/ABBA 与玩家前台门；**未**做实机验证。