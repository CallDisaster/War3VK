# ④ 产出链端到端完整；剩余二义性被精确定位 — 2026-09-18

## 1. 完整链路（全部 `src/` 路径已核实）

```
[最上游] d3d9_device.cpp:20321-20324
  auto selectedPalette = packetAuthoritativeSkinnedContractReady
      ? packet.paletteSelection
      : currentDrawSample != nullptr ? currentDrawSample->paletteSelection
                                     : skin::Selection{};      // <-- 全默认/Unknown

        :20452   selectedPalette = rebuildSelection;             （Phase 7.35 路径 2）
        :20865   if (!liveRuntimeGroupPaletteReady)
        :20866     selectedPalette = packet.paletteSelection;    （回退）
        :20867   const auto attemptedPaletteSelection = selectedPalette;
        :20868   if (skinned && ContractEnabled() && !IsSkinPaletteSelectionCurrent(...))
        :20870     selectedPalette = {};                         ★ 契约门：提交时换掉
        :21519   draw.paletteDiagnostics.source = uint32_t(selectedPalette.source);
        :21520-21523  ...slot / captureSerial / publicationTicket / frameTag
        :21524   if (InputsEnabled()) { draw.inputSkinSelection = selectedPalette; ... }

[D 点]  d3d9_war3_shadow.cpp:5249-5251
          const PaletteObjectSource s = MapPaletteObjectSource(uint32_t(diagnostics.source));
        :5245-5246  MakePaletteObjectFrames(..., frameTag, frameTag != 0u)   ⇒ nativeKnown
        :5252   NoteDrawn(key, s, ..., frameTag == 0u)            ⇒ selectionClearedByNativeOverride

[wire]  palette-object/v1 的 Drawn 事件携带 source / slot / captureSerial / publicationTicket
```

## 2. 剩余二义性（精确定位）

观测：**live `Drawn`（`terminal==NoTerminal`）3072 条，`source` 100% `Unknown`。**

由 `:20321-20324` 可知，"全 Unknown"对应两种可能：

| # | 可能 | 含义 |
| --- | --- | --- |
| 1 | `packetAuthoritativeSkinnedContractReady == false` **且** `currentDrawSample == nullptr` | 两条上游**都没提供样本** ⇒ 场景未触发该路径（**倾向此解释**，但未证实） |
| 2 | 两者之一存在，但其 `paletteSelection` **本身即** Unknown | 指向更上游的产出方 |

**要分辨二者，需要**：

```
(a) 一个已知会触发蒙皮选材的场景（当前缺）—— 首选；
(b) 或对 :20321 的三分支插桩/加一个诊断计数（例如"哪一支被选中"），
    随 palette-object 证据导出 ⇒ 一次实机采集即可分辨。
```

**(b) 是可执行的代码侧动作**，但它需要一次构建 + 实机采集 + 恢复，属于完整实验周期。

## 3. 目标④ 的最终状态

| 项 | 状态 |
| --- | --- |
| 第一半：构建启用严格契约 | ✅ 已完成并核实（19 处活调用） |
| 第二半·契约逻辑层 | ✅ 6 用例 / 33 checks 全绿（覆盖四子问题） |
| 第二半·实机对照层 | ⏳ **通道正确、测量完成、含义确认**；结论：本场景 live Drawn `source` 100% `Unknown` |
| 已排除的解释 | inputs 通道（kind=2）；解析器误读；语义混用；契约门清空；`NoSource` 神秘来源 |
| 剩余二义性 | "无样本" vs "有样本但为 Unknown" —— 需 (a) 已知触发场景 或 (b) 插桩 |

## 4. 我不做的判断

- **不**断定"产出方有缺陷"；
- **不**断定"场景不含蒙皮对象"；
- **不**为了"让 ④ 看起来完成"而选一个解释填进去。

**"没有观察到" ≠ "观察到没有"。**

## 5. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```