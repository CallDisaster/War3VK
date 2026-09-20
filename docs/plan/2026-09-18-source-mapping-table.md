# ✅ 映射规则查明：D 点 `source` 测量的含义被确认 — 2026-09-18

## 1. 映射器实现（`src/d3d9/war3/tools/war3_palette_object_capture.h:101-117`）

```cpp
inline PaletteObjectSource MapPaletteObjectSource(uint32_t ordinal) noexcept {
  switch (ordinal) {
  case uint32_t(PaletteObjectSourceOrdinal::CapturedWriter):
    return PaletteObjectSource::DrawTimeCaptured;
  case uint32_t(PaletteObjectSourceOrdinal::CapturedRawArena):
  case uint32_t(PaletteObjectSourceOrdinal::LegacyGlobalSlot):
  case uint32_t(PaletteObjectSourceOrdinal::LegacySlotCache):
    return PaletteObjectSource::ArenaSlot;
  case uint32_t(PaletteObjectSourceOrdinal::OwnedPartSnapshot):
    return PaletteObjectSource::OwnedPartSnapshot;
  case uint32_t(PaletteObjectSourceOrdinal::PoseGroups):
  case uint32_t(PaletteObjectSourceOrdinal::CModelGroups):
    return PaletteObjectSource::PoseKernel;
  default:
    return PaletteObjectSource::Unknown;      // <-- skin::Unknown(0) 落这里
  }
}
```

## 2. 由此可以确认的事

| # | 结论 |
| --- | --- |
| 1 | 映射是**显式**的（round 252 已确立），且**有损**：`CapturedRawArena`/`LegacyGlobalSlot`/`LegacySlotCache` 三种合并为 `ArenaSlot`；`PoseGroups`/`CModelGroups` 合并为 `PoseKernel`。这是按设计做的证据域合并 |
| 2 | **`skin::Source::Unknown(0)` 落入 `default` ⇒ 映射为 `PaletteObjectSource::Unknown`** ⇒ 读方打印 `'Unknown'` |
| 3 | 因此我们观测到的**主导值 `Unknown` = "该 draw 的 `selectedPalette` 没有真实来源"** ⇒ **round 251 的解读成立**（不是测量假象、不是语义混用） |

## 3. 一个仍未解释的细节（明确留待后续）

导出里除了压倒性的 `Unknown`，还有**少数 `NoSource`**（A 的 `Drawn` 里 32/3172，B 里 2/2445）。

但 **`MapPaletteObjectSource()` 从不返回 `PaletteObjectSource::None`**（它只返回上面那 5 种 + `Unknown`）。

⇒ 那少数 `NoSource` **不可能来自这个映射器** ⇒ 来自**另一条写入 `source` 的路径**
（例如其它 `NoteXxx` 调用点，或另一个 D 点调用点）。

**我未追查**，如实记为未解释项。**不猜。**
（注意：这与 round 249 的教训一致 —— 不要用"看起来合理"的解释填空。）

## 4. 目标④ 小结（截至本轮）

| 项 | 状态 |
| --- | --- |
| 第一半：构建启用严格契约 | ✅ |
| 第二半·契约逻辑层 | ✅ 6 用例 / 33 checks 全绿 |
| 第二半·实机对照层 | ⏳ **通道已找对（D 点）**，首份测量完成：`source` 几乎全 `Unknown`，A/B 无本质差异；映射含义已确认 |
| 已排除的错误通道 | inputs 导出（记录 `kind=2` 的 draw，从不写 `inputSkinSelection`） |
| 已证伪的疑点 | `source` 语义混用（存在显式映射器） |
| 未解释项 | 少数 `NoSource` 的来源；`slot`/`frameTag` 未被解析器暴露 |

## 5. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```