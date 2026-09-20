# 目标④ A/B 实机对照结论：**本场景无检验力**（两者均 100% 未设）— 2026-09-18

## 1. 实验

| | run A | run B |
| --- | --- | --- |
| 驱动 | `live_contrast_palette_objects.py`（120 s） | 同 |
| `DXVK_WAR3_SKIN_PALETTE_CONTRACT` | **"1"（开）** | **"0"（关）** |
| `DXVK_WAR3_FRAME_EVIDENCE_INPUTS` | "1" | "1" |
| 导出（inputs） | `cpu-10976-…inputs.json` 47.6 MB | `cpu-5204-…inputs.json` 47.3 MB |
| 恢复 | `RESTORE OK True` ✅ | `RESTORE OK True` ✅ |

（两次都是**独立进程** —— 因 `ContractEnabled()` 用 `static const` 锁存，中途改 env 无效。）

## 2. 结果

```
A contract=1 : draws=23629  noSel=0   source={0:23629}  slot={UINT32_MAX:23629}  agc={0:23629}
B contract=0 : draws=23459  noSel=0   source={0:23459}  slot={UINT32_MAX:23459}  agc={0:23459}
```

| 对比项 | 结论 |
| --- | --- |
| `skinPaletteSelection` **分布** | **完全一致** —— 两者 100% 都是"未设"（`source/space/domain=Unknown`、`slot=UINT32_MAX`、`actualGroupCount=0`） |
| draws 总数 | 23629 vs 23459 —— **运行间波动**，非契约效应 |
| 字段缺失 | 两次都 `noSel=0` ⇒ 通道完整 |

## 3. 结论：这个 A/B **对 ④ 的问题是"无检验力"的**

④ 要回答的是「**正常对象是否被误伤**」以及「选中什么数据 / 为何允许 / 提交时是否被换掉」。

而在本场景下：

```
两种配置里**都不存在**携带真实选材的"正常对象"
⇒ 没有任何对象可以被"误伤"
⇒ 开/关契约**都观察不到差异**
```

⇒ **这不是"契约无效果"的证据，而是"本实验无法检验该问题"的证据。**

**必须这样表述**，否则会变成又一次"没有观察到"被说成"观察到没有"。

## 4. 为什么会这样（两种可能，未区分）

| # | 可能 | 判别方法 |
| --- | --- | --- |
| 1 | 本驱动播放的**地图/画面不触发**蒙皮选材路径（例如没有需要 palette 的蒙皮单位） | 换一张确定含蒙皮单位的地图 |
| 2 | 产出方（writer）**在本构建里不填充** `skinPaletteSelection`（真实缺陷） | 在一个**已知会触发**的场景观察；或用单元/合成路径验证 writer |

**我尚未区分这两者**，因此不下结论。

注意可能 2 若为真则是**严重问题**（契约因此永远不会放行任何东西）—— 但这需要"已知会触发的场景"作为前提，
而那正是当前缺的。

## 5. 已有的间接证据（不足以定案，但值得记）

`war3_skin_palette_admission_test.exe` 的 6 个用例**全绿**（33 checks），其中包含
`normal admission and count-short refusal` 与 `shared producer-group-count admission rule`。
⇒ **契约逻辑本身**在合成输入下工作正常；问题只可能在"生产输入是否真的满足契约"。

## 6. 下一轮（明确）

```
① 找出一个**已知会触发蒙皮选材**的场景/地图（不是随便一张图）。
   线索：仓库文档里提到过"真实蒙皮选择入口"；也可从 PaletteObjectSource 的
   CapturedWriter / OwnedPartSnapshot 等来源反查谁在填充 Selection。
② 在那里重跑 A/B（契约 1 vs 0），才能得到有检验力的对照。
③ 在此之前，**不声称 ④ 第二半完成**。
```

## 7. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读分析 + 两次实机采集；临时脚本已删除）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```