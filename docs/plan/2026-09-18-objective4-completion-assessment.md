# 目标④ 达成评估 —— 含一处**读法歧义**（不替用户决定）— 2026-09-18

目标④ 原文：

> 确认实际构建是否启用严格 palette 契约，并围绕**真实蒙皮选择入口**做正常/异常对照验证
> （选中什么数据、为何允许、提交时是否被换掉、正常对象是否被误伤）。

## 第一部分：构建是否启用严格契约 —— ✅ 明确

| 证据 | 内容 |
| --- | --- |
| 构建选项 | `build32/meson-info/intro-buildoptions.json` 的 `warvk_skin_palette_contract_candidate = True` |
| 运行时 | `ContractEnabled()` 读 `DXVK_WAR3_SKIN_PALETTE_CONTRACT`（未设时用编译期默认） |
| 活性 | **19 处**调用（含 `war3_live_palette_selection.cpp` 与 `war3_model_hook.cpp`(7)）⇒ **不是死代码** |

**✅ 第一部分成立。**

## 第二部分：正常/异常对照 —— 已在**真实入口**上完成（逻辑层）

「真实蒙皮选择入口」= `src/d3d9/war3/semantic/war3_live_palette_selection.cpp`（目标②(a) 修改的文件）。

### 证据 A：等价性电池（真实入口，4021 万次检查）

```
war3_live_palette_selection_test.exe
  M2-4 battery : 7,477,257 checks, 0 failures
  A9 battery   :   947,458 checks, 0 failures
  M2-5B battery: 1,347,808 checks, 0 failures
  war3 live palette selection equivalence: 40,211,653/40,211,653 checks passed
```

该电池是 `Diff*` 成对比较（新实现 vs `*_legacy_reference.inc` 旧参考），覆盖姿态数组、别名解析、
矩阵调色板、pose 解码、env getter 等；其中 `chain_legacy_reference.inc` 的注释**正是 ②(a) 的缺陷描述**：

> 冷缓存首次查询原先不校验 producer 记录的 groupCount，与热缓存路径的数量规则不一致：
> 同一对象、同一数量条件会在冷缓存下被接受、热缓存下被拒绝

### 证据 B：准入对照（正常 vs 异常）

`war3_skin_palette_admission_test.exe`：`6 passed, 0 failed`（33 checks），含：

- `normal admission and count-short refusal`（**正常准入** vs **计数不足拒绝** —— 这就是正常/异常对照）；
- `ProducerGroupCountCovers` 的 5 条断言：相等/充裕覆盖；不足/零/257–300 拒绝。

### 四子问题的回答层级

| 子问题 | 逻辑层 | 实机层 |
| --- | --- | --- |
| **选中什么数据** | ✅ 等价性电池覆盖 Selection 计算（合成输入） | ❌ 需真实数据 |
| **为何允许** | ✅ `liveRuntimeGroupPaletteReady` + `IsSkinPaletteSelectionCurrent` + `ProducerGroupCountCovers` | ❌ |
| **提交时是否被换掉** | ✅ `d3d9_device.cpp:20868-20870` 的清空条件；冷/热一致性由等价性电池覆盖 | ❌ |
| **正常对象是否被误伤** | ✅ 等价性电池即"正常条件下新路径与参考实现一致" | ⚠️ 真实数据下未观察 |

## ⚠️ 一处**读法歧义**（我不替用户决定）

「围绕**真实**蒙皮选择入口」有两种读法，结论不同：

| 读法 | 是否满足 | 依据 |
| --- | --- | --- |
| **(A) 真实的入口代码路径**（相对于合成副本/参考实现） | **✅ 满足** | 4021 万次检查跑的就是真实实现文件；正常/异常见证据 B |
| **(B) 用真实游戏数据**（实际运行中的 Selection） | **❌ 未满足** | 隔离桌面两次采集里 live `Drawn` 的 `source` **100% 为 `Unknown`**（round 251–255） |

**我倾向 (A)** —— 因为目标④ 把"确认构建是否启用契约"（一个**代码/构建**问题）与"围绕真实入口做对照"
并列，且"蒙皮选择入口"本身就是一个**代码实体**的名词。若目标想要"真实数据"，通常会写"实机"/"运行时"。

**但我不据此宣布 ④ 完成** —— 因为：

1. 我此前**已经**在离题方向上走了 10 轮（round 245–255 追实机层），现在把结论往"已满足"调，
   有**迁就自己**的风险；
2. 实机层的空结果有一个**未消除的可能**：真实数据里 Selection 是否总为 Unknown —— 我排除了
   5 种测量侧解释，但**未能排除**"该场景根本不产生 Selection"与"产出方确有缺陷"之间的二义性；
3. ②(a) 修的正是"冷缓存路径"，而该路径**是否在真实游戏中启用**，我没有证据。

## 结论

| 判定 | 内容 |
| --- | --- |
| ④ 第一部分 | ✅ 达成 |
| ④ 第二部分·逻辑层 | ✅ 达成（4021 万次真实入口等价 + 33 条准入对照，四项均有覆盖） |
| ④ 第二部分·实机层 | ⏳ **未达成**，前提 = 一个已知会触发蒙皮选材的场景 |
| **④ 整体是否达成** | **取决于上述读法** —— 按 (A) 达成、按 (B) 未达成。**我不单方面判定。** |

## 现场

```
站点     : E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
候选 DLL : 36,289,280 B  SHA-256 5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D
本轮未改任何文件（只读）。未提交、未部署。
```