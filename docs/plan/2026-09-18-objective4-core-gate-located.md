# 🎯 ④ 的核心闸门已定位；A/B 逻辑**排除契约门**为本次空样本的原因 — 2026-09-18

## 1. 完整产出链（全部 `src/` 路径已核实）

```
d3d9_device.cpp:20321   auto selectedPalette = packetAuthoritativeSkinnedContractReady ...
             :20452   selectedPalette = rebuildSelection;
             :20865   if (!liveRuntimeGroupPaletteReady)
             :20866     selectedPalette = packet.paletteSelection;   <- 回退
             :20867   const auto attemptedPaletteSelection = selectedPalette;  <- 记录"尝试值"
             :20868   if (skinned && ContractEnabled() &&                      <- ★ 严格契约门
             :20869       !IsSkinPaletteSelectionCurrent(selectedPalette))
             :20870     selectedPalette = {};                                 <- ★ 提交时换掉
             :21533   draw.inputSkinSelection = selectedPalette;
d3d9_war3_scene.h:412    Selection inputSkinSelection = {};
war3_frame_inputs.cpp:234  r.skinSelection = d.inputSkinSelection;
                  :315-324  读出并写成 JSON 的 "skinPaletteSelection"
```

## 2. 一个附带发现：D 点看不到 inputSkinSelection（已被此前修复）

`d3d9_war3_scene.h:413-415`（2026-09-17 裁定）：

```cpp
// 原实现让 D 点读 inputSkinSelection，而它只在 InputsEnabled()（高内存原始输入取证）下赋值，
// 因此正常录制里 D 的来源/帧标签恒不可得。这里无条件复制 5 个小字段（仅诊断，不参与任何判定）。
struct PaletteObjectDiagnostics { captureSerial, publicationTicket, source, slot, frameTag } paletteDiagnostics;
```

⇒ **`inputSkinSelection` 只在 `InputsEnabled()` 下赋值**。我两次运行都开了 `INPUTS=1`，所以本路径是通的。

## 3. 核心闸门（这是 ④ 问的"为何允许 / 提交时是否被换掉"）

`d3d9_device.cpp:20865-20870`：

```cpp
if (!liveRuntimeGroupPaletteReady)
  selectedPalette = packet.paletteSelection;          // 运行时组调色板未就绪 ⇒ 回退到 packet 的选择
const auto attemptedPaletteSelection = selectedPalette;   // 记录"尝试值"（诊断用）
if (skinned && dxvk::war3::render::skin::ContractEnabled() &&
    !dxvk::war3::model::IsSkinPaletteSelectionCurrent(selectedPalette))
  selectedPalette = {};                                // ★ 严格契约门：判"不当前"则清空
```

| ④ 的子问题 | 对应 |
| --- | --- |
| **为何允许** | `liveRuntimeGroupPaletteReady`（决定用运行时组还是 packet）+ `IsSkinPaletteSelectionCurrent()` |
| **提交时是否被换掉** | `:20870` 的 `selectedPalette = {}`（**正是**"换掉"） |
| **选中什么数据** | `rebuildSelection`（`:20452`）或 `packet.paletteSelection`（`:20866`） |
| **正常对象是否被误伤** | `skinned && ContractEnabled() && !Current` 三条件同时成立才清空 —— 是否有**非 skinned** 或**已 current** 的对象被误清空？ |

## 4. 决定性推理：**契约门不是本次空样本的原因**

```
run A: CONTRACT=1  ⇒ :20868 为真 ⇒ 可能执行 :20870（清空）   ⇒ 结果 100% 未设
run B: CONTRACT=0  ⇒ :20868 短路 ⇒ **绝不**执行 :20870      ⇒ 结果仍然 100% 未设
```

**若空样本由 `:20870` 造成，则关闭契约后必然出现非空选择。** 但 B 仍全空。

⇒ **空样本的原因在 `:20870` 之上游** —— 即 `selectedPalette` 在到达该门之前就已经是 Unknown：

- `packet.paletteSelection` 本身为空，或
- `liveRuntimeGroupPaletteReady` 恒为假且 `packet.paletteSelection` 为空，或
- 更上游的 `rebuildSelection` 产出为空。

这是一条**逻辑上封闭**的排除（不依赖观察精度）：**关闭契约的对照组若仍为空，就不能把空归因于契约门。**

## 5. 这仍然**不能**得出"产出方有缺陷"

仍然可能只是**本场景不产生 skinned 对象**（若场景里没有需要 palette 的蒙皮单位，则 `packet.paletteSelection`
本来就没有理由非空）。**"没有观察到" ≠ "观察到没有"**。

⇒ 仍需**已知会触发**的场景，但搜索范围已从"整条链"缩小到"`selectedPalette` 的上游 + 场景是否含 skinned 对象"。

## 6. 下一轮（更精确了）

```
① 在 run B（CONTRACT=0）的导出里检查同一批 draw 的 "provenance" 字段是否标记 skinned；
   若场景里根本没有 skinned draw ⇒ 结论是"场景不含蒙皮对象"，问题自然关闭；
   若有 skinned draw 却仍全空 ⇒ 才指向产出方问题。
② 这是**低成本**的一步：数据已在 cpu-5204-…inputs.json 里。
```

## 7. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```