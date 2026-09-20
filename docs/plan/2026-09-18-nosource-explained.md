# ✅ `NoSource` 已解释：它全部来自**终态事件**（stage 回填为 Drawn）— 2026-09-18

## 1. 现象与解释

按 `(source, terminal)` 分组 run A 的 `Drawn` 事件（`cpu-10976-63759690553-1.json`）：

```
source=Unknown   terminal=NoTerminal      3072    <-- 真实 live Drawn
source=Unknown   terminal=WindowExpired     68    <-- 终态（stage 被回填为 Drawn）
source=NoSource  terminal=WindowExpired     32    <-- 终态（source 保持默认 None）
```

⇒ **所有 `NoSource` 都出现在 `terminal = WindowExpired` 的事件上。**

⇒ 它们是**终态事件**：`war3_palette_object_evidence.h:258-259` 对 `!sawFirstSight` 的条目把终态 stage
回填为 `Drawn`，而终态记录的 `source` 保持默认 `PaletteObjectSource::None`（`h:679-681` 的默认值）。

⇒ **不是"另一条写 `source` 的路径"**，而是**终态事件的默认值**。

（这与 round 239 的发现一致：终态会被兜底写成 `Drawn` —— 那正是待做的"写方修正③"要处理的问题。）

## 2. 于是得到一条干净的事实

```
真实 live Drawn 事件（terminal == NoTerminal）: 3072 条
其中 source == Unknown                      : 3072 条  (100%)
```

⇒ **在本场景里，到达 D 点的每一次真实绘制，其 `selectedPalette` 都没有真实来源。**

这不是测量假象（映射器已查明、读方表正确）、不是语义混用（已证伪）、
不是契约门清空的（B 关契约仍全 Unknown）、也不是"看错了通道"（D 点是正确通道）。

## 3. 这仍**不能**推出"产出方有缺陷"

仍然可能只是：**本场景没有需要 palette 的蒙皮单位** ⇒ `selectedPalette` 本来就没有理由非空。

**"没有观察到" ≠ "观察到没有"** —— 这一条在整个 ④ 的调查里被反复确认。

要跨越它，必须有**一个已知会触发蒙皮选材的场景**。**这是 ④ 剩余工作的唯一前提。**

## 4. 目标④ 的最终清单（截至本轮）

| 项 | 状态 |
| --- | --- |
| 第一半：构建启用严格契约 | ✅ |
| 第二半·契约逻辑层（6 用例 33 checks） | ✅ |
| 第二半·实机对照层 | ⏳ **通道已找对、测量已做、含义已确认**；结论：本场景 live Drawn 的 `source` 100% `Unknown` |
| 已排除 | inputs 导出通道（记录 kind=2）；`source` 语义混用；`NoSource` 的神秘来源 |
| 唯一前提 | **一个已知会触发蒙皮选材的场景** |

## 5. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读分析；临时脚本已删除）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```