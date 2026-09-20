# 🎯 定案：④ 的 A/B **在结构上不可能观察到选材**（导出记录的是 `kind=2` 的 draw）— 2026-09-18

## 1. 线索：`provenance[0]` 是**种类标签**

`inputEvidenceProvenance` 有四个不同的初始化模板（`[0]` = 种类）：

| `[0]` | 位置 | 用途 |
| --- | --- | --- |
| `1u` | `d3d9_device.cpp:21525` | 主路径（`{1u, runtimeModelPtr, 0u, 0u, ..., skinned(idx 9), ...}`）|
| **`2u`** | **`d3d9_device.cpp:23304`** | "The safe current-frame producer" |
| `3u` | `d3d9_device.cpp:27232` | 另一路径 |
| `4u` | `d3d9_device.cpp:45673` | 另一路径 |

导出样本：

```
sample: ['2','0','65536','2506','0','2506','0','0','0',...]
         ^^^        ^^^^^  ^^^^     ^^^^
         [0]=2      [2]=1<<16=65536  [3]==[5]
```

**精确匹配 `:23304` 的模板**：`{2u, 0u, 1u<<16, entry.frameSerial, 0u, m_war3ShadowPersistentFrameSerial}`
（`pv[2]=65536=1<<16` ✅、`pv[3]==pv[5]` ✅）。

## 2. 决定性证据：`kind=2` 的 draw **不设置** `inputSkinSelection`

`d3d9_device.cpp:23299-23311`：

```cpp
War3ShadowCasterDraw draw = {};            // <- 独立的 draw，全新构造
draw.mapEpoch = ...; draw.deviceEpoch = ...;
// The safe current-frame producer is a first-class final caster source.
if (war3::tools::evidence::InputsEnabled())
  draw.inputEvidenceProvenance = {2u,0u,1u<<16,entry.frameSerial,0u,m_war3ShadowPersistentFrameSerial};
// ...以下设置 shadowRenderablePart / nativeLightEmitterGeneration / shadowLayerIndex ...
// **没有 draw.inputSkinSelection = selectedPalette;**
```

而 `inputSkinSelection` 在全树的写入点**只有**：

```
d3d9_device.cpp:21533   draw.inputSkinSelection = selectedPalette;      <- kind=1 路径
               :21695   draw.inputSkinSelection = {};                  <- 清空
               :22136/:22145  注释
```

⇒ **`kind=2` 的 draw 不可能携带蒙皮选材。**

## 3. 于是 A/B 结果被完全解释

```
inputs 导出记录的是 kind=2 的 draw（war3_frame_inputs.cpp:233  r.provenance = d.inputEvidenceProvenance）
kind=2 从不写 inputSkinSelection ⇒ 恒为默认 {}
⇒ skinPaletteSelection 恒为 source=Unknown / slot=UINT32_MAX / actualGroupCount=0
⇒ 与 DXVK_WAR3_SKIN_PALETTE_CONTRACT 无关 ⇒ 两次运行必然一致
```

**这解释了 run A / run B 的 100% 未设**，而且**不是**「场景不含蒙皮对象」（我上一轮的猜测方向），
**也不是**产出方缺陷、**更不是**契约无效果。

⇒ **是测量工具的限制**：我观察的那个导出，记录的是**一类不会携带该字段的 draw**。

## 4. 更正我在 round 249 的说法

| round 249 我说 | 更正为 |
| --- | --- |
| "`r.provenance` 与 `inputEvidenceProvenance` **不是同一个数组**" | **错**。`war3_frame_inputs.cpp:233` 明写 `r.provenance = d.inputEvidenceProvenance;` ⇒ **是**同一个数组 |
| "我的索引映射没有根据" | 结论对，但**理由错**：`[9]=skinned` 只对 **`kind=1`** 的模板成立；导出里的 draw 是 `kind=2`，其模板长度/含义不同 |

⇒ 我上一轮"记为未判别"的判断**救了我一次**（没有把错结论写进项目事实），
但**我给的错误理由**也差点变成一条假事实。**"未判别"要配"为什么未判别"，而后者同样需要证据。**

## 5. ④ 的正确下一步（被本轮改写）

```
要观察蒙皮选材，必须拿到 **kind=1** 的 draw（:21525 路径，:21533 写 inputSkinSelection）。

问题：inputs 导出为何只记录 kind=2？查 war3_frame_inputs 的 draw 收集点，
      确认 kind=1 的 draw 是否/如何进入该 ring。

若不经过该 ring ⇒ 需要另找观察通道（例如已有的 skin-selection 证据、
或 :21519-21523 的 paletteDiagnostics，它是**无条件**赋值的 5 字段小载荷）。
```

**注意**：`:21519-21523` 的 `paletteDiagnostics`（`source/slot/captureSerial/publicationTicket/frameTag`）
是 **`kind=1` 路径上无条件赋值的**（不受 `InputsEnabled()` 限制）—— 这可能是比 inputs 导出**更合适**的观察通道。
**这是下一轮的首要候选。**

## 6. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```