# 阶段 C 第 1 步 · 完成：`NoteReject` 预算预检提前 —— 两个红点均已**正确**解决 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。A（doc-r1）、B（门禁恢复）已完成。
> 本文件关闭阶段 C 第 1 步留下的两个红点。**没有放宽任何断言**。

## 1. 红点 2（构建状态与源码不一致）⇒ 已解决，**是我的工具错误**

```
源文件 : 09/17 18:04:54   42021 B
exe    : 09/18 21:35:00  1267561 B   <- 比源码新
```

**根因**：用 `fs.copyFileSync`（Windows `CopyFileW` **保留源时间戳**）从 doc-r1 纯净副本还原源码
⇒ mtime 回到 09/17，**旧于 .obj** ⇒ `ninja` **正确地**认为最新、从未重建 ⇒ 陈旧产物留着我的编辑。
已核对：全树只有一份该源文件，meson `:1519-1521` 引用的正是它 ⇒ **不是依赖表损坏**。

**修正**：还原源码后把 `LastWriteTime` 置为当前，重建 ⇒ `exe 含 DIAG? False`、`checks=38`。

**新规矩（跨轮）**：**用保时间戳的复制"还原源码"会让增量构建静默跳过重建** ⇒
还原后必须置 mtime 为当前（或强制重建），并**核对产物内容**，而不只看退出码。

## 2. 红点 1（成本测试终态预留）⇒ 已解决，**缺陷在夹具，断言一字未改**

### 2.1 决定性测量

加入临时诊断（已移除，产物已核对不含 DIAG）：

```
reserveConsumed=0    emitted=0     terminalEmitted=0     droppedTerminalReserve=0  watchCount=0
reserveConsumed=448  emitted=448   terminalEmitted=224   droppedTerminalReserve=0  watchCount=0
reserveConsumed=512  emitted=512   terminalEmitted=256   droppedTerminalReserve=0  watchCount=0
```

### 2.2 解开矛盾的**关键读数**：`emitted` 是**全体**计数

```cpp
912: void EmitUnchecked(const PaletteObjectEventRecord& record, bool terminal) {
914:   m_counters.emitted++;            // 计数**全部**事件（普通 + 终态）
916:   m_counters.terminalEmitted++;    // 终态是其**子集**
```

⇒ `emitted=448, terminalEmitted=224` 的真实含义是：**普通事件只有 224 个**（448 次 `NoteReject` 中
**224 次被每帧预算拒发**），终态 224 个。`droppedTerminalReserve=0` 与"每个成功的 `NoteObjectGone`
必发终态"**完全自洽** —— 我先前的困惑源于把 `emitted` 误当"仅普通事件"。

### 2.3 根因：夹具的分帧让每帧塞 128 个键（预算 64）⇒ 恰好一半被拒

```cpp
// 原实现（每帧 128 个键，而 kPerFrameBudget = 64）
rec.NoteReject(key, R1, Frames(10u + i / kPerFrameBudget));
```

⇒ 该循环**实际只消耗了 `reserveConsumed/2` 条预留**，与它自己的注释
（"先消耗 reserveConsumed 条终态预留"）不符。

**它为什么以前"看起来"成立**：修复前"被预算拒发也照样建条目"，于是 `NoteObjectGone`
仍能对每个键发出终态 —— **一个缺陷掩盖了另一个缺陷**。本阶段 C 的修复让两类入口都在
`Insert` 前过预算，掩蔽消失，夹具自身的缺陷才暴露出来。

### 2.4 处置：**修夹具使其意图成立，而不是放宽断言**

```cpp
// 2026-09-18 阶段 C：每个键一个独立帧（使本循环真正做到"消耗 N 条预留"）
rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(10u + i));
```

**断言一字未改**，`checks` 仍为 **38**（未增未减）：

```
CLOSE reserveConsumed=0   emittedTerminals=512  droppedTerminalReserve=512   emittedBeforeClose=1024
CLOSE reserveConsumed=448 emittedTerminals=64   droppedTerminalReserve=960   emittedBeforeClose=1920
CLOSE reserveConsumed=512 emittedTerminals=0    droppedTerminalReserve=1024  emittedBeforeClose=2048
COST_VERDICT=PASS checks=38 failures=0
```

对照旧断言 `emittedTerminals == kTerminalReserve - reserveConsumed`：
**512-0=512 ✅、512-448=64 ✅、512-512=0 ✅** —— 旧断言的**本意是对的**，现在它被**真正**满足。
`emittedBeforeClose` 由 1472 → **1920** = 448(拒绝) + 448(终态) + 1024(填充)，
独立证明预消耗现在真的消耗了 448 条预留。

## 3. 阶段 C 第 1 步的最终状态（全绿）

```
ninja -C build32 -n : no work to do
meson               : Ok: 85  Fail: 0        （由 84/1 恢复）
AutoTest 全量静态    : 259 scripts, 0 failed
evidence test       : SUMMARY: 25 passed, 0 failed
wire roundtrip      : CHECKS=1141 FAILURES=0   CERTIFIED_SPEC_SATISFIED
候选 DLL            : 36,289,280 B
                      SHA-256 881B0325D89B5F8F7ADB8457514CEB4799E9D7CFD17B045C8EA26B1B5E417928
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

**本步的四项交付**：
1. `war3_palette_object_evidence.h`：`NoteReject` 新顺序 `去重 → 表满 → 非终态预算预检 → Insert → 发射`；
2. `war3_palette_object_evidence_test.cpp`：新增 `Case 25`（+ 反向变异探针证明其载荷有效）；
3. `test_semantic_build_thread_gate_static.py`：锚点更正 + 两条**顺序**断言；
4. `war3_palette_object_evidence_cost_test.cpp`：夹具分帧修正（断言未改）。

## 4. 不声称

- **不**声称阶段 C（链型 / v4 / `ObservationClosed` / 跨帧判序）已完成 —— **它才刚开始**：
  本步只做了"两类入口共用准入规则"这一条（`CanEmit` 先于 `Insert`）；
- **不**声称 K1（我引入的误拒不变量）、K2（未拒绝链被标 `Recovered`）、K3（混合链/跨帧语义）已解决；
- **不**声称"（257,300）拒绝"之类的旧结论正确（已在 doc-r1 中更正）；
- 兼容性未变：wire 版本仍由 `firstSightUsed()` 动态选 2/3，**尚未改成恒定 v4**。
