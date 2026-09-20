# 阶段 C 第 1 步 · 续：红点 2 已解决；红点 1 已决定性收窄 — 2026-09-18

## 1. ✅ 红点 2 解决：**是我的工具用法错误，不是构建系统问题**

```
源文件 : 09/17/2026 18:04:54   42021 B
exe    : 09/18/2026 21:35:00  1267561 B   <- 比源码**新**
源含我的断言? False      exe含我的断言? True      exe含原始断言? False
```

**根因**：我用 `fs.copyFileSync`（Node）从 doc-r1 纯净副本恢复该源码，
而 Windows `CopyFileW` **保留源文件时间戳** ⇒ 还原后的源码 mtime 回到 **09/17 18:04**，
**比 09/18 的 `.obj` 更旧** ⇒ `ninja` **正确地**认为目标最新、从未重建 ⇒ 陈旧产物留着我的编辑。

**⇒ 不是 Ninja 依赖表损坏，也不是 meson 引用了别的源文件**（已核对：全树只有一份该源文件，
meson `:1519-1521` 引用的正是它）。

**修正**：把源码 `LastWriteTime` 置为当前，重建：

```
[2/2] Linking target src/d3d9/war3_palette_object_evidence_cost_test.exe
ninja -C build32 -n  =>  no work to do
exe 含我的断言? False      exe 含原始断言? True       (checks=38，与源码一致)
```

**教训（已记入跨轮清单）**：**用保时间戳的复制来"还原源码"会让增量构建静默跳过重建** ——
还原源码后必须把 mtime 置为当前（或强制重建），并**核对产物内容**（不只是退出码）。

## 2. 🔍 红点 1 决定性收窄：缺陷在**成本测试的预消耗循环**，不在 `CloseWindow`

**上下文**：`CloseWindow` 的每个条目在 `:279` 走 `CanEmit(true)`，其终态封顶为
`m_counters.terminalEmitted >= kTerminalReserve`（`kTerminalReserve = 512`，`:130`）。

**实测（源码/产物已一致后的干净结果）**：

```
reserveConsumed=0    emittedTerminals=512  dropped=736?  -> dropped=512
reserveConsumed=448  emittedTerminals=288  dropped=736
reserveConsumed=512  emittedTerminals=256  dropped=768
emittedBeforeClose :  1024 / 1472 / 1536   （= 448*0? 见下）
```

**反推关窗瞬间的 `terminalEmitted`**（封顶 512 减去关窗期间发射数）：

| reserveConsumed | 关窗发射 | ⇒ 关窗时 terminalEmitted | 比值 |
| --- | --- | --- | --- |
| 0 | 512 | **0** | 0 |
| 448 | 288 | **224** | **448/2** |
| 512 | 256 | **256** | **512/2** |

⇒ **预消耗循环执行了 N 次，却只产生了 N/2 个终态。**

预消耗循环（`war3_palette_object_evidence_cost_test.cpp:663-669`）：

```cpp
for (uint32_t i = 0u; i < reserveConsumed; ++i) {
  const PaletteObjectKey key = MakeKey(0x810000u + i);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(10u + i / kPerFrameBudget));
  rec.NoteObjectGone(key);
}
```

**两个候选原因（下一步必须分辨）**：
1. **`NoteObjectGone` 只在某些条件下发终态**（例如要求 `sawDraw`/`sawServed`），
   于是奇数/偶数键之一不发 —— 这需要读 `NoteObjectGone`（`:582`）的实现；
2. **`MakeKey` 在 `i` 相差 2 时产生相同键**（若某个字段用了 `i` 的低位掩码），
   于是每两个 `i` 命中同一条目、第二次是重复观测而不再发终态。
   `MakeKey` 用了 `part & 0xFFu` 作为 rawcode —— 在 `reserveConsumed=512` 时，
   `i` 与 `i+256` 的 rawcode 相同，但 `renderablePart` 不同（`0x810000u+i` 唯一）⇒ 键仍唯一。

⇒ **候选 1 更可能。** 这也解释了 `emittedBeforeClose` = `reserveConsumed` + 1024：
预消耗的 `NoteReject` **确实**各发了 1 个普通事件（所以 N 个 `i` 贡献 N 个），
但 `NoteObjectGone` 只发了一半的终态。

## 3. 因此对红点 1 的处置方向（下一轮执行）

**不是**修改或放宽成本测试的期望，而是**先搞清 `NoteObjectGone` 的终态条件**：

1. 读 `NoteObjectGone`（`war3_palette_object_evidence.h:582`）与其调用的终态发射条件；
2. 判定"N 次循环只有 N/2 个终态"是**成本测试的夹具缺陷**（它对预留的假设错了），
   还是**`NoteObjectGone` 的真实契约**（那就必须写进文档，并据此重写断言）；
3. 只有在 1–2 明确后，才把成本测试的断言改成**正确的不变量**；
4. **并**保留对本次修复的锁定（"存活条目 ⇒ 已发链首"），使该修复在任何新断言下仍被约束。

## 4. 本轮状态

```
已解决：红点 2（构建状态与源码不一致）—— 我的工具错误，已修正并核对产物内容
已收窄：红点 1（成本测试终态预留）—— 定位到预消耗循环只产生 N/2 个终态
仍红  ：meson 84 Ok / 1 Fail = war3_palette_object_evidence_cost（2 处具名失败）

已核实绿：war3_palette_object_evidence_test 25/0；wire roundtrip CHECKS=1141 FAILURES=0；静态 259/0
候选 DLL：881B0325D89B5F8F7ADB8457514CEB4799E9D7CFD17B045C8EA26B1B5E417928
站点：F275545BAA65A015…（基线，未部署）。git：无写操作。
```
