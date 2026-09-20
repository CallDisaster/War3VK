# 完整套件失败的**分诊结果** — 2026-09-18（7 静态 + 2 meson）

> 承接 `2026-09-18-full-suite-result-and-r1-equivalence-failure.md`。本文把它们**分类归因**，并记录本轮的修复。

## 1. 分诊表

| # | 门禁 | 归因 | 状态 |
| --- | --- | --- | --- |
| 1 | `test_semantic_build_thread_gate_static.py` | **我的批次 3 改动**：该门禁断言头块**字面**写 `version=2`，而批次 3 改为 `firstSightUsed() ? 3 : 2` | **已修复**（门禁接受两种显式写法，仍拒绝 `version=1`）→ EXIT=0 |
| 2 | `test_war3_live_palette_selection_equivalence_static.py` | R1 路由变更（已确证、机制已闭合） | 待"有意差异登记" |
| 3 | `test_war3_live_palette_motion_equivalence_static.py` | 同上（同一差分电池的包装器） | 同上 |
| 4 | `test_war3_palette_a9_migration_equivalence_static.py` | 同上 | 同上 |
| 5 | `test_war3_palette_m2_5b_equivalence_static.py` | 同上 | 同上 |
| 6 | `test_war3_palette_taxonomy_equivalence_static.py` | 同上 | 同上 |
| 7 | `test_war3_palette_m2_4_equivalence_static.py` | **同为 R1 根因**：其 `fail()` 报告 `移植后 env 组 default 失败 exit=1` ⇒ 它**驱动同一测试二进制**（`war3_live_palette_selection_test`）并在 `default` env 组下因同一差分而退出 1 | 随 #2 一并处理 |
| M1 | `warvk:war3_live_palette_selection`（meson） | 同 R1 路由变更 | 随 #2 一并处理 |
| M2 | `warvk:war3_palette_object_wire_roundtrip`（meson） | 疑为 **env 注入差异**：显式设 `DXVK_WAR3_FRAME_EVIDENCE=1` + `..._PALETTE_OBJECT=1` 时 **116/116 PASS** | **根因未判定**（可能既有） |

### 1.1 依据（同源证据）

#2/#3/#4/#5/#6 的输出**完全一致**地包含：

```
M2-4 battery: 7477257 checks, 0 failures
A9 battery:     947458 checks, 0 failures
M2-5B battery: 1347808 checks, 0 failures
DIFF line 2181 random: module=0 legacy=1
```

⇒ 它们**共用同一套差分电池**（固定场景表全绿、差异只在随机世界），故为**同一根因**。
修好一处"有意差异登记"，预计可同时解决 5 个静态 + 1 个 meson。

## 2. 本轮已完成的修复（门禁同步）

`AutoTest/test_semantic_build_thread_gate_static.py` 断言由：

```python
assert re.search(r'result\["version"\]\s*=\s*2\s*;', HEADER_JSON_CODE)
```

改为接受**两种显式写法**（字面 `2`，或 `PaletteObjectRecorder().firstSightUsed() ? 3 : 2`），
并保留"不得再写 `version=1`"的约束；**注释中写明依据**（批次 3 裁定）。验证：**EXIT=0 通过**。

## 3. 剩余工作（明确）

1. **实施有意差异登记**（解决 #2–#6 + meson M1）：在随机差分世界的比较中登记允许的差异形态与依据；
2. **判定 #7（m2_4）根因**：读其 line 306 的断言内容；
3. **判定 M2（meson roundtrip）根因**：确认是否为 env 注入差异（若是，属既有问题，需单独记录而非算作本轮回归）。

## 4. 现状

| 项 | 值 |
| --- | --- |
| 静态失败 | **7 → 6**（本轮修复 1 个） |
| meson 失败 | 2（未变） |
| 未提交 / 未部署 | 是 |
| 现场 DLL | `F275545B…`（`519AFA69…` 备份在位） |
| 目标 | **不可标记完成** |
---

## 5. 后续更新（2026-09-18 本轮）

### 5.1 #7 归因已确定

`test_war3_palette_m2_4_equivalence_static.py:305-306` 的 `fail()` 抛出信息为
`移植后 env 组 default 失败 exit=1`；其 `PROBE_MATRICES` 只有 `default` 与 `flipped` 两组 env。
⇒ 该门禁**驱动 `war3_live_palette_selection_test` 二进制**，失败来自该二进制在同一差分电池上的非零退出，
**与 #2 同根因**，并非独立缺陷。

### 5.2 因此"同源"实际覆盖 6 个静态门禁 + 1 个 meson 目标

```
#2 selection_equivalence      (DIFF 证据)
#3 motion_equivalence         (DIFF 证据)
#4 a9_migration_equivalence   (DIFF 证据)
#5 m2_5b_equivalence          (DIFF 证据)
#6 taxonomy_equivalence       (DIFF 证据)
#7 m2_4_equivalence           (同一二进制 exit=1)
M1 meson war3_live_palette_selection (同一二进制)
```

⇒ **修好一处"有意差异登记"，预计同时解决 7 个门禁中的 6 个静态 + 1 个 meson。**
（第 7 个静态 `semantic_build_thread_gate_static` 已在上一轮修复。）

### 5.3 仍未判定

- **M2**：meson `war3_palette_object_wire_roundtrip` 失败（显式设两个 env 时 116/116 PASS）⇒ 需判定是
  "meson 未注入 env" 这一**既有**条件，还是本轮引入的回归。判定方法：查该目标的 `test(...)` 是否带 `env:`，
  以及检查点 c0 之前的 meson 记录是否也为该目标失败。

### 5.4 现状

| 项 | 值 |
| --- | --- |
| 静态失败 | **6**（7 → 6，已修 `semantic_build_thread_gate_static`） |
| meson 失败 | 2 |
| 归因为 R1 的 | **6 静态 + 1 meson** |
| 未判定 | meson `war3_palette_object_wire_roundtrip` |
| 未提交 / 未部署 | 是 |
| 目标 | **不可标记完成** |