# ✅ 疑点被证伪：`source` **没有**语义混用缺陷（存在显式映射器）— 2026-09-18

## 1. 我上一轮的怀疑

round 251 我看到 `d3d9_war3_shadow.cpp:5251` 把 `uint32_t(draw.paletteDiagnostics.source)`（一个
`skin::Source` 值）传进 `NoteDrawn`，而 `NoteDrawn` 的形参是 `PaletteObjectSource`；
于是记录「**读方对 D 点 `source` 的解释可能是错的。** …**可能是又一处真实缺陷**」。

## 2. 核实结果：**怀疑不成立**

`d3d9_war3_shadow.cpp:5247-5259`（完整调用）：

```cpp
// 上级 03:58：来源取**该次 draw 实际携带的按值诊断来源**，不得从最近一次 Served 推测；
// native override 清空了语义 palette 时必须记 None + flag（本次 draw 未消费语义 palette）。
const war3::tools::evidence::PaletteObjectSource paletteObjectDrawSource =
    war3::tools::evidence::MapPaletteObjectSource(          // <-- 显式映射器
        uint32_t(draw.paletteDiagnostics.source));
war3::tools::evidence::PaletteObjectRecorder().NoteDrawn(
    paletteObjectKey, paletteObjectDrawSource,
    war3::tools::evidence::PaletteObjectHitKey(
        paletteObjectDrawSource, draw.paletteDiagnostics.slot,
        draw.paletteDiagnostics.captureSerial,
        draw.paletteDiagnostics.publicationTicket),
    paletteObjectFrames,
    draw.paletteDiagnostics.frameTag == 0u);
```

⇒ **存在一个显式的 `MapPaletteObjectSource()`**，专门做 `skin::Source` → `PaletteObjectSource` 的转换。
⇒ **不存在语义混用**；读方按 `PaletteObjectSource` 解读是从映射器产出的值 ⇒ **读方是对的**。

**我上一轮的"可能是真实缺陷"是错的。**

## 3. 顺带确认了两个 D 点语义（此前不清楚）

| 位置 | 语义 |
| --- | --- |
| `:5245-5246` | `MakePaletteObjectFrames(..., frameTag, frameTag != 0u)` ⇒ **`nativeKnown = (frameTag != 0)`** |
| `:5259` | `selectionClearedByNativeOverride = (frameTag == 0u)` |

⇒ D 点的 native 帧已知性**完全由诊断载荷的 `frameTag` 决定**；
`frameTag == 0` 时同时记 `nativeUnknown=true` 与 `selectionClearedByNativeOverride=true`。
（这与目标②(b) 在 S 点固定 `nativeKnown=false` 是同一语义方针的两处体现。）

## 4. 因此 round 251 的观测结论**仍然成立且更可信**

```
A contract=1 : Drawn=3172  Drawn.source={Unknown:3140, NoSource:32}
B contract=0 : Drawn=2445  Drawn.source={Unknown:2443, NoSource:2}
pre-fix      : Drawn=2018  Drawn.source={Unknown:2016, NoSource:2}
```

既然映射器存在且读方表正确，那么：

- **`source` 几乎全 `Unknown` 是真实观测**，不是测量假象；
- 结合 `frameTag` 驱动 `nativeKnown`，可推断这些 draw 的 `frameTag == 0`
  ⇒ 同时伴随 `nativeUnknown=true` 与 `selectionClearedByNativeOverride=true`；
- 结合"B 关契约 ⇒ 绝不清空" ⇒ **`selectedPalette` 在到达契约门之前就已是 Unknown**（结论不变）。

## 5. 这一轮的意义

我上一轮**提出了一个怀疑，并明确标注为"未判定"**；本轮**核实后把它证伪**。

**这正是应该发生的过程**：

| 若我上一轮直接写成"发现真实缺陷" | 则会留下一条**假的严重缺陷记录**，后续可能有人去"修"一个不存在的问题 |
| 我实际做的 | 标"未判定" → 本轮查形参类型 → 发现映射器 → **明确证伪并记录** |

⇒ **"提出怀疑"与"断言缺陷"是两件事**；前者只需证据线索，后者需要闭环核实。

## 6. 仍未做的

- 未查看 `MapPaletteObjectSource()` 的实现（它能确认每个 `skin::Source` 各映射到哪个 `PaletteObjectSource`，
  从而解释"少数 `NoSource`"的来源）——**这是下一步的低成本动作**；
- 未扩展解析器以暴露 `slot`/`frameTag`（round 251 的"限制 1"仍存在）；

## 7. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```