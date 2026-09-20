# 步骤 ① 完成：R1 的数量规则已抽为单一实现并被独立证明 — 2026-09-18

> 外部复审要求「修错误时，应证明新行为符合预期」。本文记录该**证明**及其带来的结构改善。

## 1. 做了什么

R1 的缺陷能存在，根源是**同一条规则在两处各写一份**：热缓存写 `producerGroupCount >= requiredPaletteCount`，
冷缓存原先根本没有。因此把规则抽为**单一纯谓词**：

```cpp
// src/d3d9/war3/render/war3_skin_palette_selection.h
inline bool ProducerGroupCountCovers(uint32_t required,uint32_t producerGroupCount) noexcept {
  return required!=0u&&producerGroupCount>=required;
}
```

**冷/热两处都改为调用它**（原先的字面比较已全部消失）。

## 2. 为什么这构成"证明新行为符合预期"

| 层次 | 证据 |
| --- | --- |
| **行为** | `war3_skin_palette_admission_test` 新增案例 6「共享数量准入规则」：**6 checks / 0 failures**；其中明确断言 `ProducerGroupCountCovers(10,9) == false` —— 即 **R1 的目标行为：差一个也必须拒绝** |
| **结构** | 批次 2 静态门禁改为断言 `ProducerGroupCountCovers(` 在选择链中**恰好出现 2 次**，且 `producerGroupCount < requiredPaletteCount` 与 `producerGroupCount >= requiredPaletteCount` 两种字面比较**都不得再出现** ⇒ 规则只维护一份（复审表「相同规则是否只维护一次」项） |

### 2.1 一处必要澄清（我最初的断言写错了）

我最初断言 `ProducerGroupCountCovers(257,300) == false`（以为它管上界），实测为 `true`。
**该函数只管覆盖关系**；上界由调用方把关（选择链在 `requiredPaletteCount > 256u` 早退，严格准入函数在 `required > 64` 早退）。
已把断言改正为 `true` 并在注释中写明分工 —— **测试表达的是实际契约，不是我的预期**。

## 3. 全量重建后的验证

```
ninja -C build32            -> 完成（26 个目标，含公共头文件改动引发的重编译）
ninja -C build32 -n         -> no work
war3_skin_palette_admission_test   EXIT=0  SUMMARY: 6 passed, 0 failed
war3_palette_object_evidence_test  EXIT=0  SUMMARY: 24 passed, 0 failed
war3_palette_object_wire_roundtrip EXIT=0  ROUNDTRIP checks=116 failures=0 PASS
war3_palette_object_evidence_cost  EXIT=0  all checks passed
test_independent_review_sep18_fixes_static.py  EXIT=0 PASS（含同步后的共享规则断言）
test_first_sight_observation_chain_static.py   EXIT=0 PASS
```

## 4. ⚠️ 一处读数纠正（我自己的）

在验证命令中我曾用 `py ... | Select-Object -First 2` 取样，得到的 `EXIT=0` 是**假象**：
`Select-Object -First` 会**提前终止上游管道**，使 `$LASTEXITCODE` 不再反映真实退出码。

**等价门禁仍然失败**（DIFF 行仍在：`module=4294967295 legacy=4` 与 `module=8 legacy=4294967295`），
因为它对照的参照实现**不含 R1 的规则**。本轮**没有**修改任何门禁期望，也没有处理该基线问题。

## 5. 进度

```
① 独立测试证明新行为符合预期            ✅ 本轮完成（规则行为 6/6 + 结构断言）
② 生成器快照可得性                      ✅ 已确认（全部在位）
③ 按 B1 实施具名补丁 + 同步 SHA 断言      ⬜ 未开始
④ 同一检查点重跑全部门禁                 ⬜ 未开始
```

## 6. 现状与边界

| 项 | 值 |
| --- | --- |
| 静态失败 | 6（未变化；等价基线问题未处理） |
| meson 失败 | 2 |
| 门禁期望 | **仍未修改** |
| 未提交 / 未部署 | 是 |
| 现场 DLL | `F275545B…`（`519AFA69…` 备份在位）；未改用户视频设置 |
| 目标 | **不可标记完成** |