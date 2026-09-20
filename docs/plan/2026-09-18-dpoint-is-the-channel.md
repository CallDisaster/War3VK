# 🎯 ④ 的观察通道 = D 点（不是 inputs 导出）；首份测量与两项限制 — 2026-09-18

## 1. 为什么 D 点是正确通道

`paletteDiagnostics`（`d3d9_war3_scene.h:416-422`）在 `d3d9_device.cpp:21519-21523` **无条件赋值**，
由 **D 点 `NoteDrawn`**（`d3d9_war3_shadow.cpp:5245-5259`）消费：

```cpp
d3d9_war3_shadow.cpp:5251  uint32_t(draw.paletteDiagnostics.source));
                    :5255  paletteObjectDrawSource, draw.paletteDiagnostics.slot,
                    :5256  draw.paletteDiagnostics.captureSerial,
                    :5257  draw.paletteDiagnostics.publicationTicket),
```

⇒ **`palette-object/v1` 的 `Drawn` 事件携带这些诊断字段**，且在**主导出**里（不需 INPUTS）。
与上轮结论一致：inputs 导出记录 `kind=2` 的 draw（从不写 `inputSkinSelection`），**结构上看不到选材**。

## 2. 首份测量（复用已在磁盘的三份导出，无需新采集）

```
run                     palette events   Drawn 数   Drawn.source 分布
A  contract=1                3691          3172     {Unknown:3140, NoSource:32}
B  contract=0                3691          2445     {Unknown:2443, NoSource:2}
   earlier pre-fix           3691          2018     {Unknown:2016, NoSource:2}
```

## 3. 可以说

1. 在 D 点（正确通道）上，`source` 在三次运行里都**压倒性地是 `Unknown`**；
2. A（契约开）与 B（契约关）的 `source` 分布**没有本质差异**；
3. 结合"B 关闭契约 ⇒ `:20868` 短路 ⇒ 绝不执行清空" ⇒ **`selectedPalette` 在到达契约门之前就已是 Unknown**。

## 4. 两项必须说清的限制（不得越过）

### 4.1 `slot` / `frameTag` 解析器**不解码**

我的首版脚本输出 `Drawn.slot={"None":...}`、`Drawn.frameTag={"None":...}` ——
**`None` 表示解析器没暴露这两个字段**，不是"值为 None"。

⇒ **那两项测量无意义**，明确标注，避免被当成"slot/frameTag 为空"的证据。
⇒ 要看它们须先扩展解析器或直读 wire 槽位 —— **未做**。
⇒ 故 ④ 的"**选中什么数据**"目前**只回答了 `source` 一维**。

### 4.2 `source` 存在**语义混用**（新待查项）

| 枚举 | 定义 |
| --- | --- |
| `skin::Source`（`war3_skin_palette_selection.h:12-13`） | `Unknown=0, CapturedWriter=1, CapturedRawArena=2, OwnedPartSnapshot=3, LegacyGlobalSlot=4, LegacySlotCache=5, PoseGroups=6, CModelGroups=7` |
| `PaletteObjectSource`（`war3_palette_object_evidence.h:65-68`） | `None=0, ArenaSlot=1, ProducerSnapshot=2, PoseKernel=3, DrawTimeCaptured=4, PublishedRegistry=5, OwnedPartSnapshot=6, Unknown=7` |
| 解析器 `SOURCES`（`analyze_palette_object_evidence.py:290`） | 与 **`PaletteObjectSource`** 一致 |

而 `d3d9_war3_shadow.cpp:5251` 传的是 **`uint32_t(draw.paletteDiagnostics.source)`**，
它是 **`skin::Source`** 的值（`d3d9_device.cpp:21519`）。

⇒ **一个 `skin::Source` 的值被放进按 `PaletteObjectSource` 解读的槽位。**
两枚举都是 8 项、都有 `Unknown`，但**序号含义不同**（`skin::Unknown=0` vs `PaletteObjectSource::Unknown=7`）。

⇒ **读方对 D 点 `source` 的解释可能是错的。** 本轮未判定是否为缺陷（需确认 `NoteDrawn` 形参类型），
但**必须成为一项待查**：

```
① 查 NoteDrawn 的 source 形参类型：PaletteObjectSource，还是 uint32_t 强转？
② 若形参是 PaletteObjectSource，则 skin::Source 的值被错误解读 ⇒ 真缺陷；
   若形参是 uint32_t 且已声明"此处放 skin::Source 序号" ⇒ 是读方表用错。
③ 无论哪种，读方 SOURCES 与写方实际值域的对应关系**必须写死在文档里**。
```

## 5. ④ 状态更新

| 项 | 状态 |
| --- | --- |
| 第一半（构建是否启用严格契约） | ✅ |
| 第二半·契约逻辑层 | ✅ 6 用例 33 checks 全绿 |
| 第二半·实机对照层 | ⏳ **通道已找对**（D 点）；首份测量：`source` 几乎全 `Unknown`，A/B 无本质差异 |
| inputs 导出通道 | ❌ **结构上看不到选材**，已排除 |

## 6. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读分析；临时脚本已删除）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```