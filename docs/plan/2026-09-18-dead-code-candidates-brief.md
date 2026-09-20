# 死代码候选决策简报（2026-09-18 主线程；**只读发现，未实施任何删除**）

> 背景：本夜 A9/A10 裁定已落地（A10 删除 / A9 迁出）。T3 只读审计期间又发现**三个无调用者函数**，与 A9/A10 同型。
> 性质：**决策简报**，供上级裁定；**本文件不授权也不包含任何删除动作**。

## 1. 候选清单

| 函数 | 位置 | 读点 | `src` 内调用者 |
| --- | --- | --- | --- |
| `QueryBlendedPaletteBySlotIndexExact` | `war3_model_hook.cpp:9110`（声明 `war3_model_hook.h:461`） | `:9123`、`:9153` | **无** |
| `QueryBlendedPaletteBySlotIndexBestEffort` | `war3_model_hook.cpp:9166`（声明 `war3_model_hook.h:481`） | `:9179`、`:9220` | **无** |
| `ValidateBlendedPaletteBySlotIndexExact` | `war3_model_hook.cpp:9073` | `:9085` | **无**（仅被 `…Exact` 调用） |

## 2. 证据（本夜实测，均可复现）

1. **无调用者**：ripgrep `src/` 全量，三者除自身定义/声明与彼此调用外**无任何调用点**（连测试也不调用真实现——宿主测试用**自有替身**，例见 `war3_live_palette_selection_test.cpp:2781` 自行定义 `QueryBlendedPaletteBySlotIndex`）；
2. **无取地址**：`&QueryBlendedPaletteBySlotIndex` 等取地址模式**命中 0**；
3. **无导出**：`src/d3d9/d3d9.def`（46 行，权威导出清单）**不含**这些符号；
4. **AutoTest 侧只是清单条目**：三者出现在 `AutoTest/data_collection_entrypoints.json` 中，但那是 **`WARVK_DATA_SCOPE` 插桩点清单**，由 `AutoTest/test_data_collection_static.py` 核对 ⇒ **登记 ≠ 调用**；
5. **有静态门禁钉住其结构**：`AutoTest/test_current_draw_trusted_palette_direct_pack_static.py:10-23` 断言 `ValidateBlendedPaletteBySlotIndexExact` 存在且在 copy body 中**校验先于 `Pack…`**。

## 3. 需要上级注意的**矛盾**（本夜已如实登记，未擅自解释）

- `war3_model_hook.cpp:9061-9070` 的注释自述这是 **Phase 7.34 重写**，并称旧行为是"用户观察到的『**只显示一个部位 + 有闪烁**』的**直接原因之一**"；
- **但**该函数族在 `src` 内没有调用者 ⇒ 张力：**一个被记录为修复用户可见缺陷的函数，却无静态调用点**。
- 两种可能（**均未验证**）：①它经**函数指针/导出符号/跨模块/采集层**被间接调用（取地址与导出面已排除，但**调用图未做**）；②**真正修复点在别处**（写入侧语义或 ready 仲裁），该族是被保留的历史实现。

⇒ **在解释清楚之前，不建议删除**。这与 A9/A10 不同：那两个有**明确的 0 实例化 / 0 调用证据**且不涉及"被记录为修复用户可见缺陷"的历史。

## 4. 若将来决定清理：已探明的代价（与 A9/A10 同型）

| 步骤 | 内容 |
| --- | --- |
| 1 | 从 `AutoTest/data_collection_entrypoints.json` 移除对应条目（否则 `test_data_collection_static.py` 红） |
| 2 | 更新 `test_current_draw_trusted_palette_direct_pack_static.py` 的锚点断言 |
| 3 | 新增/更新相应 **FROZEN** 登记（若删除后仍有预算门禁命名族覆盖） |
| 4 | **全量门禁复跑**（静态 256 · meson 84 · 宿主差分 · 生命周期 187 · 往返 …） |
| 5 | 落 `*-record.md` + 变异真跑（≥2 条）+ SHA 还原证明 |

## 5. 我的建议（供裁定）

1. **先做第 3 节的解释性核查**（调用图 + 采集层是否真的调用 + "修复点在别处"的可能性），**再谈删除**；
2. 若核查确认无人调用 ⇒ 按 A9/A10 的成熟流程清理（代价已探明）；
3. **无论结果如何，本夜不动**（冻结树 + 未获授权）。

## 6. 边界

- 本简报全部结论为**静态只读**（ripgrep + 读码 + `.def` 核对）；**无运行时证据**；
- **未删除、未修改任何源码**；树保持冻结（`ninja -C build32 -n` = no work）；现场 DLL `A0A51AF2…` 未替换；未 git 写、未部署。

---

## 7. ⚠️ **本简报的重大更正（同夜 09:36，主线程读码发现）**

> **请以本节为准**；第 1–5 节的事实判断有**部分错误**，保留原文以留痕。

### 7.1 我漏掉了一个函数，导致三条错误结论

静态门禁 `AutoTest/test_current_draw_trusted_palette_direct_pack_static.py`（39 行）揭示了我此前未检索的函数：

```
.h:469   声明  CopyBlendedPaletteBytesBySlotIndexExact
.cpp:9134 定义  CopyBlendedPaletteBytesBySlotIndexExact
.cpp:9146        └─ 调用 ValidateBlendedPaletteBySlotIndexExact   <- 真实生产调用！
current_draw_contract.cpp:2365  调用 CopyBlendedPaletteBytesBySlotIndexExact  <- 真实生产调用者！
```

### 7.2 更正清单

| 我在第 1–5 节的说法 | 更正后的事实 |
| --- | --- |
| `ValidateBlendedPaletteBySlotIndexExact` **无调用者** | **错误**：它有 **2 个生产调用者**（`war3_model_hook.cpp:9117` 与 `:9146`），其中 `:9146` 位于 `CopyBlendedPaletteBytesBySlotIndexExact` 内 |
| 第 3 节的"**矛盾**"（注释称修复用户缺陷却无调用点） | **矛盾不存在**：Phase 7.34 的 **validate-then-pack 逻辑是活的** —— 经 `PublishCurrentDrawContract`（`current_draw_contract.cpp:2365`）→ `CopyBlendedPaletteBytesBySlotIndexExact` → `Validate…` → `PackWar3PaletteMatrix3x4` 实际运行；是我漏了这个函数，**不是代码有问题** |
| 候选为 3 个函数 | 候选**只剩 2 个**：`QueryBlendedPaletteBySlotIndexExact`(`:9110`) 与 `QueryBlendedPaletteBySlotIndexBestEffort`(`:9166`) 仍见不到调用者 |

### 7.3 修订后的候选与建议

- **候选**：`QueryBlendedPaletteBySlotIndexExact`（`:9110`）、`QueryBlendedPaletteBySlotIndexBestEffort`（`:9166`）—— 仍无 `src` 内调用者；
- **不再候选**：`ValidateBlendedPaletteBySlotIndexExact`（有 2 个生产调用者，**属活代码**）；
- **建议不变但理由更强**：**先做调用图核查再谈删除** —— 本次教训正是"定向 grep 会漏掉中间层函数"；`CopyBlendedPaletteBytesBySlotIndexExact` 就是这么漏掉的；
- ⚠️ 第 4 节的"清理代价五步"**仅对剩下的 2 个候选适用**（其中 `test_current_draw_trusted_palette_direct_pack_static.py` 的断言**针对 `Copy…` 与 `Validate…`**，删除候选**不需要**改它）。

### 7.4 诚信留痕

- 这是本夜第 **9** 次自我纠正；也是**第一次由我主动追查自己交付物中的矛盾而发现事实错误**（而非事后偶遇）；
- 触发路径：我在第 3 节写下的"矛盾"本身不可信 ⇒ 反向去读门禁与 `current_draw_contract.cpp` 的注释 ⇒ 发现漏检函数。**写下可疑点，是发现自身错误的有效手段。**

## 8. 应用 §7.4 教训后的复核：两个候选的**清理代价不同**（新增，09:40）

按"定向 grep 会漏掉中间层函数"的教训，我对**剩余 2 个候选**做了反向检索（AutoTest 侧谁引用它们）：

| 候选 | `src` 调用者 | AutoTest 引用 | 清理代价 |
| --- | --- | --- | --- |
| `QueryBlendedPaletteBySlotIndexExact`（`:9110`） | **无** | ① `data_collection_entrypoints.json:648`（插桩清单条目）② **`test_current_draw_trusted_palette_direct_pack_static.py:11` 把它当作文本切片锚点**（`validate_end = HOOK_CPP.index("bool QueryBlendedPaletteBySlotIndexExact", validate_start)`） | **中**：删除会**打断该门禁的切片**（`:11` 找不到锚点即红）⇒ 需同步改写门禁 |
| `QueryBlendedPaletteBySlotIndexBestEffort`（`:9166`） | **无** | **零引用**（AutoTest 中完全未出现） | **低**：无门禁、无清单、无调用 ⇒ 最干净的候选 |

### 结论（供裁定用）

- 二者**都不是"随手可删"**，但也**都不构成风险**（当前无人调用 ⇒ 无论留删都不影响行为）；
- 若要做，建议**分开处理**：先删 `…BestEffort`（代价最低），`…Exact` 需连带改门禁锚点；
- ⚠️ 但仍**必须先完成 §7.3 的调用图核查**——本轮已证明"定向检索会漏中间层"，故在调用图（或等效反查）完成前，**任何删除都不应启动**；
- 另注：`…Exact` 的历史地位（Phase 7.34/7.35 "路径 1"）与实际存活链（`Copy…` + `Validate…`）**已在 §7 澄清**——真正在跑的是 `Copy…` 那条链，`…Exact` 的名字被保留在注释、清单与门禁锚点中。

### 边界

**只读（ripgrep）**；**未删除、未改任何源码**；树保持冻结（`ninja -C build32 -n` = no work）；现场 DLL `A0A51AF2…` 未替换；未 git 写、未部署。

## 9. **我出错的根本原因已查明**：该函数族**命名不一致**（09:45）

对 `PaletteBySlotIndex` 做**族级枚举**（不再按单个名字猜）后，`src` 内的全部定义如下：

| # | 定义 | 调用者 |
| --- | --- | --- |
| 1 | `QueryBlendedPaletteBySlotIndex`（`war3_model_hook.cpp:9010`） | `war3_live_palette_selection.cpp:512`（**活**） |
| 2 | `ValidateBlendedPaletteBySlotIndexExact`（`:9073`） | `:9117`、`:9146`（**活**） |
| 3 | `QueryBlendedPaletteBySlotIndexExact`（`:9110`） | **无** |
| 4 | `QueryBlendedPaletteBySlotIndexBestEffort`（`:9166`） | **无** |

### 关键发现：**族内命名不一致**

- 实际存活的中间层函数名为 **`CopyBlendedPalette` + `Bytes` + `BySlotIndex` + `Exact`**，即字符串是 **`PaletteBytesBySlotIndex`**，**并不包含连续的 `PaletteBySlotIndex`**！
- ⇒ **这就是我在 §7 漏检它的机械原因**：我用 `PaletteBySlotIndex`（以及具体函数名的近似形式）做子串检索，**插入了 `Bytes` 的词永远匹配不到**；
- ⇒ **可推广的教训（已写入本节）**：在命名不统一的代码库里，**子串枚举必须允许词间插入**（例如同时搜 `BySlotIndex` 这一更短的核心片段），否则会系统性漏掉整类函数；本夜我用 `BySlotIndex` 复核后，族成员一次列全（7 个定义 + 调用点，与逐名检索结果一致）。

### 由此确认的最终候选状态

- **候选（仍无调用者）**：`QueryBlendedPaletteBySlotIndexExact`（`:9110`）、`QueryBlendedPaletteBySlotIndexBestEffort`（`:9166`）；
- **非候选（有生产调用者）**：`QueryBlendedPaletteBySlotIndex`、`ValidateBlendedPaletteBySlotIndexExact`、`CopyBlendedPaletteBytesBySlotIndexExact`；
- **族级枚举已完成**（§9 表格即全族），因此 §7.3 要求的"调用图核查"**在本族范围内已达成**；跨族（经其它模块转发）仍未做。

### 边界

**只读（ripgrep）**；**未删除、未改任何源码**；树保持冻结（`ninja -C build32 -n` = no work）；现场 DLL `A0A51AF2…` 未替换。
