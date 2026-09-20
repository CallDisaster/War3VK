# 目标④ 实机对照 run A：测量通道可用，但本场景**不产出真实蒙皮选材** — 2026-09-18

## 1. 我先前**看错了地方**（第五次同一类教训）

我先在带标签的事件里找 `skin-selection/v1` ⇒ **0 条**，一度以为 `INPUTS=1` 没生效。

实际：它**不在带标签事件里**，而在 **inputs 导出**（`DXVK_WAR3_FRAME_EVIDENCE_INPUTS=1` 产生
`.inputs.json`）的**结构化 schema** 中：

```json
{"batches":[{"draws":[{"geometry":"...","part":"824495004","provenance":[...],
                     "skinPaletteSelection":{...}}]}]}
```

⇒ 我搜的是 `label`，而它实际是 `draws[].skinPaletteSelection` 字段。**"先确认自己在看正确的地方"** 第五次生效。

## 2. run A 参数

```
驱动   : AutoTest/live_contrast_palette_objects.py（120 s）
WARVK_EXTRA_ENV = {"DXVK_WAR3_FRAME_EVIDENCE_INPUTS":"1",
                   "DXVK_WAR3_SKIN_PALETTE_CONTRACT":"1"}    <- 契约 = 开（= 本构建默认）
导出   : cpu-10976-63759690553-1.json（7,254,952 B）
         cpu-10976-63759690553-1.json.inputs.json（47,601,896 B）
恢复   : RESTORE OK True   ✅  站点已回基线
```

## 3. 测量结果

```
batches=224   draws=23629   drawsWithoutSelection=0      <- 字段**总是存在**（schema=1）

actualGroupCount == 0 : 23629        >0 : 0
source  : {0 (Unknown): 23629}
space   : {0 (Unknown): 23629}
domain  : {0 (Unknown): 23629}
slot    : {4294967295 (UINT32_MAX, 未设): 23629}
```

## 4. 解读（严格区分"没观察到"与"观察到没有"）

### 可以说

| # | 事实 |
| --- | --- |
| 1 | `skinPaletteSelection` 在**每一个 draw 上都被写出**（23629/23629，无缺失）⇒ **测量通道可用、无遗漏** |
| 2 | 本次运行该字段**全部是"未设"形状**（source/space/domain 皆 Unknown、slot=UINT32_MAX、actualGroupCount=0） |
| 3 | `GroupRange(required, actual)` 要求两者皆非零 ⇒ 此形状下恒 false ⇒ 严格契约**正确地全部拒绝** |

### 不可以说

| 不可说 | 为什么 |
| --- | --- |
| "契约拒绝了所有对象" | 只对**本次运行/本场景**成立 |
| "契约会误伤正常对象" | 本场景根本没产出真实选材，**没有"正常对象"可谈** |
| "产出方有缺陷" | **"没有观察到" ≠ "观察到没有"** —— 可能只是本场景（驱动播放的地图/画面）不触发该路径 |

## 5. 为什么必须谨慎

要断言"产出方从不提供真实选择"，需**知道哪个场景会触发**该路径，并在那里观察到 0。
本驱动场景（120 s、固定地图）**未触发** ⇒ **空样本，不是阴性证据**。

这与读方里 `emptySample` 与 `Uncovered` 是两个不同结论同理。

## 6. 下一轮（明确）

```
① run B：同驱动同场景，仅把 DXVK_WAR3_SKIN_PALETTE_CONTRACT 改成 "0"
   （ContractEnabled() 用 static const 锁存 ⇒ 必须**新进程**）。
② 对比 A/B 的 skinPaletteSelection 分布：
   - 若两者**完全相同**（都全 Unknown）⇒ **本场景不触发该路径**，契约开关在此场景无可观测效果
     ⇒ 需**换场景/换地图**才能得到有意义的对照；
   - 若不同 ⇒ 契约开关生效，进一步分析差异。
③ 记录须写明：这是**隔离桌面的功能对照**，**不是前台性能数据**。
```

## 7. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读分析 + 一次实机采集；临时脚本已删除）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```