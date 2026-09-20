# M2-4 抽取记录：调色板 compose-policy 判定 / env getter 余项 → 模块

> 状态：**完成（§1–§12 全部闭合）**。迁移逐字节落地；legacy 参考、宿主差分
> （8 字段逐调用整数/位模式精确相等 + 每调用结构 memcmp + 段末累计 memcmp +
> 20 场景独立期望值表）、预算门禁 `FROZEN_M2_4`、独立 M2-4 等价门禁与
> **2 条变异真跑**全部闭合；全量门禁在**最终树**复跑留档。
> 本轮**不部署、不启动游戏、不 git 写、不触碰 `E:\Work` 下任何文件、不称稳定版**。
> 原始门禁输出：`docs/plan/2026-09-18-m2-4-full-gate-rerun.log`。

工单：`docs/plan/2026-09-18-overnight-execution-plan.md` §2「M2-4/M2-5（富余）」、
`docs/plan/2026-09-18-deepseek-overnight-handoff.md` §4 阶段 4 第 1 条。
设计与清册：`docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md` §1.1 表 A
（A1–A10）、§1.4 执行顺序建议、§3.3（M2-5 必须排在 M2-4 之后）、§4-2（A8 迁出方式未定）、
§4-7（A9/A10 是否真死代码）。
同构先例：`…-m2-1-…`、`…-m2-2-…`、`…-m2-3-migration-equivalence-record.md`、
`…-m2-5-taxonomy-extraction-record.md`。

## 0. 结论摘要

- device.cpp 的 **8 个符号（9 个定义：`War3SemanticPaletteLooksModelLocal` 两个重载）
  逐字节迁入**既有调色板模块 `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`。
- **唯一机械差异**：每个被删定义在 device.cpp 原位留下一行 `// M2-4: <key> -> …
  (byte-identical body)` 锚注释（与其后原有的空行并存）；函数正文本身在
  **模块 .cpp 的 BEGIN/END 标记之间逐字节保留**，由入库生成器 `--m2-4` 与 M2-4 等价
  门禁**各自独立**做双向逐字节校验（9/9 份正文相同）。
- 所有输入**显式传参**；模块内**不出现**任何 device 成员、registry / hook 全局
  （迁移脚本 fail-closed 扫描 `m_war3Scene` / `m_war3Semantic` / `m_state` /
  `VisibleRenderableRegistry::instance` / `this->`，0 命中）。
- 宿主差分：**N/N 全过**（default 37,920,643/37,920,643；M2-4 电池独占 7,477,257 条），
  8 个字段逐字段整数精确相等 + 每调用 `memcmp` + 段末累计 `memcmp`；另有
  `--probe-m2-4` 的 20 场景 × 2 组 env 独立期望值表。
- 两条变异真跑：语义常量漂移 ⇒ 差分红（42,003 条 DIFF，逐字段点名）；可编译回流 ⇒
  预算门禁点名 `War3SemanticHashMatrix4: device.cpp=1 > budget=0`。两条都完成
  「改→构建→跑→红→还原→touch→真实重编译→复跑绿」，源码 SHA-256 逐字节还原。
- 全量门禁在最终树全绿（`ninja -n` no work、构建 exit 0、预算/M1/M2/M2-3/M2-5/M2-4
  门禁 exit 0、meson 83/83、全量静态 252/252、记录器 23/23、成本 PASS(38/38)、
  生命周期 187/187、解析器静态 94/94、根读方 55/55、往返 CERTIFIED）。
- **A9 `War3SemanticBuildWorldPaletteIfNeeded` 与 A10 `War3SemanticVectorStorageReadable`
  两条死代码本轮未迁、未删**；裁定建议见 §10.1。
- **device.cpp 侧调用点编排（M2-5 表 B：B1/B2/B4/B5/B6–B11）一个字节没迁**——那是
  M2-5 的余项，不在本轮范围。

## 1. 迁出范围与落点

### 1.1 迁出清单（符号 / 行号 / 字节账）

| # | 符号（key） | pre-M2-4 行号 | 定义 B（CRLF） | 锚注释 B（CRLF） | Δ | 落点 |
| --- | --- | --- | --- | --- | --- | --- |
| A1 | `War3SemanticPaletteInPlaceAppendRuntime` | 1302–1309 | 417 | 125 | −292 | 模块 .cpp（env getter） |
| A2 | `War3SemanticDrawTimePoseRuntime` | 2079–2087 | 481 | 117 | −364 | 模块 .cpp（env getter） |
| A8 | `War3SemanticBoundsRadiusForObjectKind` | 7144–7160 | 440 | 123 | −317 | 模块 .cpp（A4 依赖的纯查表） |
| A7 | `War3SemanticTranslationDistanceSq` | 7177–7182 | 232 | 119 | −113 | 模块 .cpp（A4 supporting） |
| A6 | `War3SemanticTranslationFinite` | 7184–7187 | 152 | 115 | −37 | 模块 .cpp（A4 supporting） |
| A3 | `War3SemanticPaletteStorageReadable` | 7189–7196 | 272 | 120 | −152 | 模块 .cpp（A4 supporting） |
| A4a | `War3SemanticPaletteLooksModelLocal`（pointer 重载） | 7209–7263 | 2334 | 129 | −2205 | 模块 .cpp（compose-policy 判定） |
| A4b | `War3SemanticPaletteLooksModelLocal`（vector 重载） | 7265–7274 | 360 | 128 | −232 | 模块 .cpp（同上） |
| A5 | `War3SemanticHashMatrix4` | 7292–7301 | 437 | 109 | −328 | 模块 .cpp（哈希姊妹） |
|  | **合计** |  | **5125** | **1085** | **−4040** |  |

**字节账闭合（可复算）**：`device.cpp` 2,254,826 B → **2,250,786 B**（−4,040），
与上表合计**逐字节相等**；把 9 行锚注释换回 9 个定义可**逐字节重建** pre-M2-4 快照
（本轮以脚本实测 `reconstruct == post: True`）。
模块侧：`.h` 9,517 → **12,358 B**（+2,841，声明 + 说明注释）；`.cpp` 41,008 → **48,248 B**
（+7,240：9 份正文 + BEGIN/END 标记 + 说明注释 + `<cmath>` / `<limits>` 两个 include）。

**唯一机械差异（显式声明）**：删定义 → 插锚注释。除此之外 device.cpp 的其它 token
一个字节未动（逐行 diff 只有 9 个 hunk）。注释文本自带行尾换行，因此每个站点原有的
那一个空行**保留**（与 M2-1 的注释先例同形）。

### 1.2 落点与命名理由

落点 = **既有** `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`，不新开模块：

1. 这 8 个符号中有 6 个与已迁入该模块的 M2-1/M2-2/M2-3 内容**同一单一职责**
   （live palette 选择 / 组合判定）：A5 是清册明说「与已迁的
   `War3SemanticHashMatrixPalette` 同模块自然归位」的姊妹；A1/A2 与 M2-1 的 4 个 env
   getter **完全同型**（清册 §1.1 A1/A2 依据列）；A3/A4/A6/A7 是 palette
   compose-policy 判定与其 supporting 纯函数。
2. 同 namespace（`dxvk::war3::semantic`）、同 using-directive 解析机制、同头文件
   include 面；device.cpp 的调用点**文本一个字节未改**。
3. A8 是唯一「职责略有跨界」的符号：它是纯 `switch(objectKind)->半径` 查表，全仓
   1 定义 + 7 调用点，其中 6 个属 bounds 族。把它的定义留在 device.cpp 的匿名
   命名空间会让模块侧**无法调用**（匿名命名空间符号不导出）；把半径改成显式形参会
   改写 A4 正文、破坏逐字节等价。因此随 A4 一起迁入同一模块，**它的 6 个 bounds
   调用点文本一个字节未改**，仍经 device.cpp 既有的 `using namespace
   dxvk::war3::semantic;` 解析到模块定义（M1/M2-1 同一机制）。这属于清册 §4-2 给出的
   两个可选项之一（「把 A8 单列 M2-4 子项」），本记录显式登记该裁定。

### 1.3 签名层与正文层的差异分离（为什么必须分两层校验）

模块侧签名相对迁移前的差异**只有两类**，都不在正文里：

1. 两个 env getter 迁移前是 `inline bool …`，模块侧改为头文件声明 + .cpp 非 inline
   定义（M2-1 的 4 个 getter 先例）；
2. A4 pointer 重载的 `bool checkReadable = true` 默认实参按 M2-2 先例集中在模块 .h
   声明上，.cpp 定义不再重复（C++ 不允许同一 TU 内重复默认实参）。

因此本轮的「逐字节」以**函数正文**为单位：迁移脚本从 pre-M2-4 快照按签名锚抽取定义、
去掉签名与结尾 `}`，得到正文（LF）；模块 .cpp 在 `// --- BEGIN M2-4 body: <key> … ---`
与 `// --- END M2-4 body: <key> ---` 之间逐字节保留该正文。生成器与门禁各自独立复算。

## 2. provenance（fail-closed）与 baseline 实测

| 项 | 值 |
| --- | --- |
| pre-M2-4 工作树 `src/d3d9/d3d9_device.cpp` | **2,254,826 B** / `7391F3071F7D2CF32583C57FF1D914FC59AA765A5EDD5749F5D3A8FCBD8A0844` |
| 捕获时间（机器本地时钟） | `2026-09-18T03:34:55` |
| `%TEMP%` 备份 | `%TEMP%\device_pre_m2_4.cpp`（2,254,826 B / 同 SHA，脚本比对 `True`） |
| 交叉验证 | 该 SHA **逐字节等于** M2-5 记录 §2 的「迁移后 device.cpp」⇒ M2-4 的迁移前状态就是 post-M2-5 工作树，闭合 |
| worktree-vs-git-blob | 与前五片相同：工作树文件哈希，**不是** git blob（M1–M2-5 均未提交） |

同一次捕获的其它备份（`%TEMP%`）：`module_pre_m2_4_live_palette_selection.{h,cpp}`、
`test_pre_m2_4_war3_live_palette_selection_test.cpp`、`gen_pre_m2_4_legacy_reference.py`、
`budget_pre_m2_4_static.py`、`meson_pre_m2_4_d3d9.build`；迁移后另有
`module_post_m2_4_live_palette_selection.cpp` 与 `device_post_m2_4.cpp`（变异还原用）。
一次性迁移脚本与运行器（不入库，审计用）：`%TEMP%` 之外的工作目录
`D:\tmp\m2_4_work\`（`m2_4_migrate.py` 12,420 B /
`FD2704443228F978F76B927D3E7156DFDF90EA3EB94BD378AE35CA598CD3FD2C`，
`run_full_gates.ps1`）。迁移脚本可**从快照重放**：本轮用它把两个模块文件从 pre-M2-4
状态重建，产出与首轮**逐字节相同**的 `.h` / `.cpp`（SHA 复核见 §5）。

**baseline 实测（FROZEN_M2_4）**：用预算门禁**同一个** `DEF_RE` 与该门禁同一条
「第 0 列起始、非注释行」规则，在本门禁内**从预算门禁源文件里 eval 出同一个 regex**
（不是抄写），扫 pre-M2-4 快照，实测行首站点：

| 符号 | baseline（实测） | budget |
| --- | --- | --- |
| `War3SemanticPaletteInPlaceAppendRuntime` | 1 | 0 |
| `War3SemanticDrawTimePoseRuntime` | 1 | 0 |
| `War3SemanticTranslationFinite` | 1 | 0 |
| `War3SemanticTranslationDistanceSq` | 1 | 0 |
| `War3SemanticPaletteStorageReadable` | 1 | 0 |
| `War3SemanticPaletteLooksModelLocal` | **2**（两个重载各 1 个行首站点） | 0 |
| `War3SemanticBoundsRadiusForObjectKind` | 1 | 0 |
| `War3SemanticHashMatrix4` | 1 | 0 |

⇒ `budget = 0` 与迁移**同一次改动**落地（budget=0 只在「定义已不在 device.cpp」时成立）。
`FROZEN_M2_4` 独立成表再 `update` 进 `FROZEN`（M2-1/2/3/5 同构）：A8 在**原始** FROZEN 块里
仍保留 `(1, 1)`，因此 M1 等价门禁（只解析原始块、以 `budget<baseline` 推导自己的 26 个
COVERED）不会把 A8 误判为「M1 迁出但无 M1 等价证据」。

**命名族盲区（判定 3）**：`War3SemanticPaletteStorageReadable` /
`War3SemanticPaletteLooksModelLocal` / `War3SemanticHashMatrix4` /
`War3SemanticTranslationFinite` / `War3SemanticTranslationDistanceSq` **都不匹配**
`SEMANTIC_FAMILY_RE`（本记录用门禁同一 regex 逐个跑过：5 个全 `False`），判定 3 对它们盲；
不登记 `FROZEN_M2_4` 就无法捕到回流。A1/A2/A8 匹配命名族，也一并显式登记。

## 3. legacy 参考与双向 fail-closed 生成器（`--m2-4`）

- 新文件 `src/d3d9/war3/semantic/tests/war3_live_palette_selection_m2_4_legacy_reference.inc`
  = **9,397 B / 216 行**，
  `E9CB8931B427CB87172BCC96B03497103116DC30F1C1197D3A00FF218F2F5899`。
- 由入库生成器 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-4` 产出；
  **M1/M2-1/M2-2/M2-3/M2-5 五份 .inc 与其 SHA 一字未动**（既有门禁 exit 0 已复核）。
- 生成器 fail-closed 三步（与 M2-5 同型）：(1) 快照 SHA-256 / 字节数必须等于登记常量；
  (2) 每个定义锚必须**唯一**命中、行号必须等于登记范围、正文不得含 device/registry token；
  (3) **模块 .cpp 每个 BEGIN/END 标记之间的正文必须与该 .inc 的正文逐字节相同**
  （实测 9/9 份相同）——「参考 = 实现」不依赖人工比对。
- 命名空间改写规则（除 LF 归一外唯一的机械编辑）：外层改为 `m2_legacy_reference`；
  `.inc` 内按**依赖序**发射（A8 → A7 → A6 → A3 → A4a → A4b → A5，另有 A1/A2），
  使正文里无限定的调用（`War3SemanticTranslationFinite` /
  `War3SemanticBoundsRadiusForObjectKind` / `War3SemanticPaletteStorageReadable` /
  `War3SemanticPaletteLooksModelLocal` / `War3GetEnvU32`）解析到同命名空间的 legacy 副本
  ——与 pre-M2-4 device.cpp 经 using-directive 的真实调用目标一致。
  `dxvk::war3::render::ObjectKind` / `dxvk::war3::IsReadableRange` 是**共享真实契约原语**
  （两侧同一份头文件，刻意不重写）。正文 token **一个都没改**。

## 4. 8 个结果字段与比较口径

差分电池对每个输入调用一次，比较下列 **8 个字段**（全部整数 / 位模式精确相等）：

| 字段 | 来源 | 口径 |
| --- | --- | --- |
| `hash4` | A5 `War3SemanticHashMatrix4(world)` | uint64 精确相等 |
| `radiusBits` | A8 `War3SemanticBoundsRadiusForObjectKind(kind)` | float **位模式**（`FloatBits`） |
| `distBits` | A7 `War3SemanticTranslationDistanceSq(first, world)` | float 位模式 |
| `finiteWorld` | A6 `War3SemanticTranslationFinite(world)` | 0/1 |
| `finitePal0` | A6 `War3SemanticTranslationFinite(first)` | 0/1 |
| `readable` | A3 `War3SemanticPaletteStorageReadable(vector)` | 0/1 |
| `looksPtr` | A4 pointer 重载 `(ptr, count, world, kind, checkReadable)` | 0/1 |
| `looksVec` | A4 vector 重载 `(vector, world, kind)` | 0/1 |

另有 A1/A2 两个 env getter 与 legacy 的 0/1 对照（在 `DiffEnvGettersCurrentEnv` 与
`--probe` 的 bool 探针表里）。**每调用**除 8 条字段检查外还对整个 `M24Result` 做一次
`memcmp`（捕获「写到了这 8 个字段之外」）；**每个场景段结束**再对两侧累计结构做一次
`memcmp`。容差 = **0**。

## 5. 宿主差分等价测试

差分测试：`src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp`
（M2-1/M2-2/M2-3/M2-5 既有断言一字未动的**加法式扩展**；meson 目标名与链接源未变
⇒ meson 目标数不变）。新增 include、`DiffM24Systematic` / `DiffM24Random`、
`M2-4 battery` 计数打印、`RunProbeM24` 与 `--probe-m2-4` 模式，以及 env getter 探针 2 条。

### 5.1 场景电池

| 电池 | 规模 | 覆盖 |
| --- | --- | --- |
| 系统网格 | **630,784 次调用** | 7 个 palette variant（空 / 1 个近原点 / 4 个近原点 / 5 个含 NaN / 4 个远原点 / 3 个含 Inf / 3 个平移 500–700）× 2 种可读性注册（替身可读区间 ON/OFF）× 8 个 world（零 / `4.0001`（worldMagSq 16.0008）/ `4.0`（恰好 16）/ `1000` / `4096` / NaN / Inf / **`5.0`（worldMagSq 25，专咬 `worldMagSq > 16` 常量漂移）**）× 11 个 count（0/1/2/3/4/5/255/256/257/4096/4097）× 256 个 kind（全 uint8）× 2 个 checkReadable |
| nullptr 分支 | 22 次调用 | pointer 重载的 `palette == nullptr` 早退 × 11 count × 2 checkReadable |
| 定种子随机世界 | **200,000 次调用** | splitmix64 随机矩阵位模式（含特殊位）、随机 kind/count/checkReadable/可读性注册、`size = roll % 9` 的 vector 长度、随机 nullptr 注入 |

合计 **830,806 次两侧调用**；每次 9 条（8 字段 + 结构 memcmp）⇒ **7,477,254**，加 3 次段末累计
`memcmp` ⇒ **M2-4 电池 7,477,257 条断言**（测试自身打印，门禁按精确值 + 下限双重咬合）。

**轮内修正（如实记录）**：首版电池的 world 变体没有落在 `worldMagSq ∈ (16, 32]` 区间、
palette 变体也没有「自身 mag 仍在 1024 内、但与 world 距离超阈值」的组合，因此一条
`> 16.0f → > 32.0f` 的变异只能靠随机电池咬到 3 条 DIFF。本轮据此**补了 world=`5.0` 与
palette 平移 500–700 两个输入维度**（电池 6,666,249 → 7,477,257 条），随后同一变异给出
42,003 条 DIFF（§8.1）。这是输入域缺口，不是差分口径问题。

### 5.2 实测（最终树，四矩阵）

| env 矩阵 | 结果 |
| --- | --- |
| default | `37920643/37920643 checks passed`，EXIT=0，`M2-4 battery: 7477257 checks, 0 failures` |
| `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1` | `37920642/37920642`，EXIT=0 |
| `DXVK_WAR3_SKIN_PALETTE_CONTRACT=1` | `34432531/34432531`，EXIT=0 |
| `…CONTRACT=1` + `…DIAGNOSTICS=1` | `34432530/34432530`，EXIT=0 |

（检查数是**全文件累计**，含 M2-1/M2-2/M2-3/M2-5 既有电池。）

### 5.3 `--probe-m2-4` 独立期望值表

20 个固定场景 × 2 组 env（`default` / `in-place=0 + draw-time-pose=1`），逐场景要求
module == legacy **全部 10 个字段**相等，并与门禁内**独立登记**的期望值表逐字段核对。

- 期望值不是「抄实跑」：`hash4` 由门禁内**独立实现的 FNV-1a 64**（offset
  `0xcbf29ce484222325`、prime `0x100000001b3`，float32 位模式、行主序）复算，
  并与 C++ 侧一致（`9287544938805421157` = world `(1000,0,0)`；
  `14920258192649708645` = identity；`2931326237932479589` = world-NaN）；
  `radiusBits` 由 `ObjectKind` 底层值手工表（0=Unknown→0（调用方 fallback 260）、1=Unit 260、
  2=Building 900、3=Destructible 750、4=Item 220、5=Effect 900、255→0）复算；
  `distBits` 由 float32 步进复算（`(1000−64)²=876096`、`(1000−1000)²=0`、
  `(1e6−1000)²` 按 float32 舍入 = `1399348607` 位模式、`64²=4096`）；`looks*` 由阈值
  推导（Unit → `guardRadius = max(384, 260×1.5) = 390` → `thresholdSq = 152100`；
  `closestPaletteMagSq = 64² = 4096 ≤ max(1024, 780)² = 1048576`）。
- **诚实记录**：首版 probe 场景把 kind=2 标成 Unit 并期望 `looks*=1`，实跑为 0；按源码
  重新推导发现 `ObjectKind` 底层值是 `Unknown=0, Unit=1, Building=2, …`，kind=2 的半径是
  900 → 阈值 `1350² = 1822500 > 876096` ⇒ 期望值应为 0。**以源码重新推导后修正场景编号**
  （A4 场景统一用 kind=1），最终 20 行全部与手工期望一致才写入门禁。

## 6. 独立 M2-4 等价门禁

新文件 `AutoTest/test_war3_palette_m2_4_equivalence_static.py`（全量静态 251 → **252**）。
静态 fail-closed：证据文件齐全 / legacy provenance 与自身 SHA / 本门禁**自己**从 .inc
抽正文与模块 BEGIN/END 之间逐字节比较 / device.cpp 8 个符号行首站点为 0（用预算门禁同一
`DEF_RE`，从门禁源码中 `eval` 出来）/ 9 个锚注释存在 / **A9·A10 仍在 device.cpp** /
`FROZEN_M2_4` 键集合与 baseline·budget / 原始 FROZEN 块中 A8 仍为 `(1, 1)` /
FNV 自证 / meson 与生成器 CLI / 差分测试引用齐全。
动态：4 组 env 矩阵 exit 0 且总检查数 ≥ 各矩阵登记下限，并**按精确值**咬住
`M2-4 battery` 检查数（7,477,257）与 0 failures；`--probe-m2-4` 2 组 env 逐行对照独立期望表。

## 7. 变异验证（2 条真跑）

### 7.1 变异 A：模块侧判定常量漂移（差分必须红）

- 改：模块 .cpp 的 `if (!(worldMagSq > 16.0f))` → `> 32.0f`（A4 compose-policy 的
  world 门限）。变异后模块 `.cpp` SHA `C641C468E0D1912F5DFEFA24B45F5C159EAAA81C618042695E93B38240007FD8`。
- 构建：`NINJA_EXIT=0`（**能编译** —— 正是「语义漂移但不报错」的回归类型），
  DLL 与差分测试 exe 都真实重链（`[3/4] Linking … war3_live_palette_selection_test.exe`、
  `[4/4] Linking target src/d3d9/d3d9.dll`）。
- 跑（红）：`37878640/37920643 checks passed`，EXIT=1，**42,003 条 DIFF**，首 4 条：
  `DIFF line 3755 M24Grid: M24Result memcmp mismatch` /
  `DIFF line 3755 M24.looksPtr: module=0 legacy=1`（重复出现，逐调用）。
- 同时变红的**第二个捕手**：M2-4 等价门禁 exit 1，
  「模块 .cpp 的 `War3SemanticPaletteLooksModelLocal[pointer]` 正文与 legacy 参考正文逐字节不一致」。
- **预算门禁在变异 A 下仍是 exit 0**（155 冻结符号 / 已迁出 46 / 站点 568）——
  这正说明值级漂移只能靠差分电池咬住，静态符号门禁对此**无能力**。
- 还原：`%TEMP%` 之外工作目录的 `module_post_m2_4_live_palette_selection.cpp` 复制回 +
  `LastWriteTime` 置当前时刻；SHA `89D506FE6C7D088E180CD012EA62D5407C649920325C9A83F11FC6D3E3F0E6F5`
  **逐字节相同（MATCH True）**；真实重编译后复跑 `37920643/37920643 checks passed`、EXIT=0。

### 7.2 变异 B：职责回流（预算门禁必须点名）

- 改：在 device.cpp 的 `using namespace dxvk::war3::semantic;` 之后插入
  `namespace warvk_m2_4_reflow_probe { <War3SemanticHashMatrix4 定义逐字节> }`
  （定义行从第 0 列开始，与 M2-3/M2-5 变异同形；具名命名空间避免与 using-directive
  导入的模块声明歧义）。变异后 device.cpp = **2,251,302 B** /
  `0353B003F82E1AAC93442A51EA658773EC4F46A0289E1E727FF14B0D05D140FC`。
- 构建：`NINJA_EXIT=0`，日志尾 `[4/4] Linking target src/d3d9/d3d9.dll` ⇒ **可编译回流**
  （编译器不拦）。
- 预算门禁（红并**点名**）：exit 1，
  `d3d9_device.cpp 中语义选择职责符号超过冻结预算（职责回流）：` /
  `  War3SemanticHashMatrix4: device.cpp=1 > budget=0`。
- 独立 M2-4 门禁**也**红（exit 1）：
  `device.cpp 仍存在已迁出符号 War3SemanticHashMatrix4 的行首站点 1 个（回流/双实现）`
  ⇒ 本轮这条回流有**两个**捕手；即使 M2-4 门禁不存在，`FROZEN_M2_4` 的判定 1 仍能单独点名。
- 还原：SHA `D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811`
  **逐字节相同（MATCH True）**；真实重编译（日志尾 `[2/2] Linking target src/d3d9/d3d9.dll`）后
  预算门禁 exit 0（冻结符号 155；行首站点 迁移前 165 → 上限 112 → 当前 112；已迁出 46；
  device.cpp 站点 568）。

### 7.3 还原完整性（SHA-256 证明）

| 被变异文件 | 变异后 SHA-256 | 还原后 SHA-256 | 逐字节相同 |
| --- | --- | --- | --- |
| 模块 `war3_live_palette_selection.cpp` | `C641C468…07FD8` | `89D506FE6C7D088E180CD012EA62D5407C649920325C9A83F11FC6D3E3F0E6F5` | **是** |
| `d3d9_device.cpp` | `0353B003…140FC` | `D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811` | **是** |

**构建陷阱（M2-3/M2-5 已实测，本轮复核并规避）**：`Copy-Item` 保留源文件 mtime 会让 ninja 判
`no work`，只比源码 SHA 会得到假绿/假红。本轮还原一律显式置 `LastWriteTime`，并以构建日志里
的真实编译/链接行确认二进制来自还原后的源码。

## 8. 跟随迁移改锚的既有门禁（断言语义未削弱）

`AutoTest/test_direct_geoset_owner_handoff_static.py` 原先把扫描区间结尾锚定为
`uint64_t War3SemanticHashMatrix4`（device.cpp :7292 的 A5 定义）。A5 被 M2-4 迁出后该文本
不再存在，门禁**在 A5 迁移的第一步就崩**（`ValueError: substring not found`）；这是全量静态
首轮 252 中的**唯一**红点。

本轮改锚为它在文件里**紧随其后且未被本轮改动**的 M2-1 迁移注释锚
（`// M2-1: War3SemanticHashMatrixPalette`）：扫描区间 `[START, END)` 的边界与迁移前
**同一个文件位置**，区间内容仅多出「替换 A5 定义的那一行 M2-4 注释」。断言本身
（`cache_store < geo_ref < owner_move`、`!geosetSnapshotCacheHit` 闸、非 move 赋值不存在、
`sharedGeoset` 计数为 1）**一条未改**，且新增文本不含 `sharedGeoset` ⇒ 覆盖面不变。
门禁 exit 0。

## 9. 全量门禁复跑（最终树）

窗口：两条变异均已还原、源码 SHA 与 §2/§7.3 一致。原始输出
`docs/plan/2026-09-18-m2-4-full-gate-rerun.log`。

| # | 门禁 | 原始结果 |
| --- | --- | --- |
| 0 | 源码身份 | 见 §9.1 |
| 1 | `ninja -C build32 -n` | `ninja: no work to do.`，EXIT=0 |
| 2 | 最终树真实重建（Below Normal + `-j2`，touch 后编译+链接） | `NINJA_EXIT=0` |
| 3 | 预算门禁 | EXIT 0：`冻结符号 155；行首站点 迁移前 165 → 迁出后上限 112 → 当前 112；已迁出 46 个符号；device.cpp 行首定义站点总数 568` |
| 4 | M1 等价门禁（**一个字节不改**） | EXIT 0：`已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / SHA-256 2EA43F97…` |
| 5 | M2 等价门禁（M2-3 扩展后） | EXIT 0（M2-1 8 + M2-2 链 1 + M2-3 motion 3 全部有覆盖；probe 6 + probe-chain 10；motion-on 下限 12000000） |
| 6 | M2-3 等价门禁 | EXIT 0（motion 参考 8406 B / `7DF61688…`；probe-motion 15 场景；`motion-on 差分 37920642`（下限 12000000）） |
| 7 | M2-5 等价门禁 | EXIT 0（taxonomy 1 符号 / 34 字段单一实现；参考 22511 B / `A5333AC7…`；正文逐字节 18937 B；`diagnostics-on 差分 37920642`（下限 21000000）） |
| 8 | **M2-4 等价门禁（本轮新增）** | EXIT 0（见 §9.2） |
| 9 | `meson test -C build32 --num-processes 2` | `Ok: 83  Fail: 0`，EXIT=0（**83→83**：加法式扩展，未新增 meson 目标） |
| 10 | 全量静态 `AutoTest/test_*_static.py` 逐个 | `STATIC_TOTAL=252 STATIC_PASS=252 STATIC_FAIL=0`（**251→252**：新增 M2-4 等价证据门禁 1 个） |
| 11 | 记录器 `war3_palette_object_evidence_test` | `SUMMARY: 23 passed, 0 failed`，EXIT=0 |
| 12 | 成本 `war3_palette_object_evidence_cost_test` | `COST_VERDICT=PASS checks=38 failures=0`，EXIT=0 |
| 13 | 生命周期 `war3_shadow_build_lifecycle_test` | `SUMMARY: … 187/187 case(s) passed`，EXIT=0 |
| 14 | 解析器静态 `AutoTest/test_palette_object_evidence_analysis_static.py` | `Ran 94 tests … OK`，EXIT=0 |
| 15 | 根读方 `AutoTest/test_analyze_frame_evidence.py` | `Ran 55 tests … OK`，EXIT=0 |
| 16 | 往返 `py AutoTest/test_palette_object_wire_roundtrip.py <exe>` | `CHECKS=1107 FAILURES=0`；`ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`（A–F）；EXIT=0 |
| 17 | 差分四矩阵 | §5.2（全部 EXIT=0） |
| 18 | `--probe-m2-4` 两组 env | §5.3（全部 EXIT=0） |
| 19 | DLL 身份 | `36277956 B / BDB47E4E238CE9D794EA251EF533F6EA6CA29F833D77214D318E989340396851`（§9.3） |
| 20 | 复跑 `ninja -C build32 -n` | `no work to do.`，EXIT=0 |

**一个需要点名的新事实（与 M2-5 同类）**：M2-3 门禁打印的 `motion-on 差分` 与 M2-5 门禁打印的
`diagnostics-on 差分` 数字都是**全文件累计**检查数，M2-4 电池把它们从 30,443,383 抬到
37,920,642。两条门禁的登记下限（12,000,000 / 21,000,000）仍满足，M2-3/M2-5 的语义覆盖
不受影响，但这两个数字**不再等价于**各自的专项覆盖量。门禁本身未改。

### 9.1 源码与产物身份

| 项 | 值 |
| --- | --- |
| `src/d3d9/d3d9_device.cpp`（迁移后） | **2,250,786 B** / `D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811` |
| `src/d3d9/war3/semantic/war3_live_palette_selection.h` | **12,358 B** / `CAF54E2A6EB6E739C0012902D59DCE69CA0BF92CE187F321E9845759A78C23C9` |
| `src/d3d9/war3/semantic/war3_live_palette_selection.cpp` | **48,248 B** / `89D506FE6C7D088E180CD012EA62D5407C649920325C9A83F11FC6D3E3F0E6F5` |
| `…/tests/war3_live_palette_selection_m2_4_legacy_reference.inc` | **9,397 B** / `E9CB8931B427CB87172BCC96B03497103116DC30F1C1197D3A00FF218F2F5899` |
| `…/tests/war3_live_palette_selection_test.cpp` | **157,497 B** / `7893D30708D4B6F0DE6683D84167BC0D721A03C54F0FDDEEE6EBE7DE24103B01` |
| `AutoTest/test_war3_palette_m2_4_equivalence_static.py`（新增） | **29,033 B** / `4FC51870F07BA7AAA2D8F8EDABBBF37CB9815BE45077338629C1E559BBA7074D` |
| `AutoTest/gen_war3_live_palette_selection_legacy_reference.py`（加 `--m2-4`） | **47,265 B** / `4061DE6AE2184B0610F7F0ECD27C9793CC74C4218513B6D62039E2A3343C804F` |
| `AutoTest/test_device_semantic_responsibility_budget_static.py`（加 `FROZEN_M2_4`） | **23,028 B** / `44C31B72E8C900099EEA56AD2692BBA88DA09C7EC9F27398CDD1FA7E5B5CB602` |
| `AutoTest/test_direct_geoset_owner_handoff_static.py`（改锚） | 见 §9 日志（内容随本次改动） |
| `build32/src/d3d9/d3d9.dll` | **36,277,956 B** / `BDB47E4E238CE9D794EA251EF533F6EA6CA29F833D77214D318E989340396851` |
| `build32/src/d3d9/war3_live_palette_selection_test.exe` | **6,526,211 B** / `7D6305055846B37F3635939C718E195A751F7851FD75C62DF9D1A2B3ECA6ED7E` |

### 9.2 M2-4 门禁自报

`war3 palette m2-4 equivalence gate static checks passed（M2-4 已迁出符号 8 个 / 定义 9 份；
legacy 参考 9397 B / SHA-256 E9CB8931B427CB87…；正文逐字节（门禁独立复算）；
pre-M2-4 device.cpp 2254826 B / 7391F3071F7D2CF3…；probe-m2-4 场景 20 × 2 组 env；
M2-4 电池下限 7400000（登记精确值 7477257））`。

### 9.3 DLL 身份与可复现性

最终产物 `build32/src/d3d9/d3d9.dll` = **36,277,956 B**，
SHA-256 `BDB47E4E238CE9D794EA251EF533F6EA6CA29F833D77214D318E989340396851`
（最终树最后一次真实构建：touch 源码后日志出现 `[1/6]`–`[5/6]` 真实编译与 `[6/6] Linking
target src/d3d9/d3d9.dll`）。对照 M2-5 的最终 DLL（36,272,891 B / `34FB05C5…`）⇒ 尺寸
**+5,065 B**：device.cpp 净减 4,040 B 源码、模块 .cpp 净增 7,240 B 源码（含 `<cmath>`/`<limits>`），
另加一次真实重链的链接时间戳。

**同尺寸不同 SHA 的实例（本轮再次实测）**：同一最终源码先后产出 `C71D5D16…`（首次构建）、
`EE21E637…`（第一次全门禁 touch 后真实重链）与 `BDB47E4E…`（文档写入后复跑的真实重链），
三者**同为 36,277,956 B**、SHA 各不相同。
**未部署**：本轮没有覆盖任何游戏目录文件，未启动游戏或编辑器，
`E:\Work` 下未触碰任何文件。

> 诚实标注（沿用 M2-5 §9.1 的实测）：同一源码在两次链接后得到**同尺寸、不同 SHA** 的 DLL
> —— MinGW `ld` 默认写入链接时间戳（PE 头 `TimeDateStamp` 等）。因此「同源码 ⇒ 同尺寸」
> 成立、「同源码 ⇒ 同 SHA」**不成立**。本记录登记的是最终树**最后一次真实构建**的身份。

## 10. 未覆盖项与裁定建议（如实声明）

### 10.1 A9 / A10 死代码的裁定建议（本轮**未迁、未删**）

本轮实测（与清册 §1.1 一致）：`War3SemanticBuildWorldPaletteIfNeeded`（`[[maybe_unused]]`）
与 `War3SemanticVectorStorageReadable`（function template）全 `src` 各只出现**定义这一处**，
0 调用 / 0 实例化；两者**都不匹配**语义命名族，也都不占 `DEF_RE` 行首站点（A10 的 `template`
行与 A9 的 `[[maybe_unused]]` 前缀都不以标识符开头 ⇒ 预算门禁的站点计数为 0）。
本轮已用门禁把它钉成「**未被擅自删除**」（M2-4 门禁的 `DEAD_CODE_STILL_IN_DEVICE` 断言）。

**裁定建议（本轮不做，留待单独改动）**：

1. `War3SemanticVectorStorageReadable`（A10）：**建议删除**。它是 A3 的早期泛型版本，
   迁出后已无任何调用者；删除不会改变编译产物（模板不会实例化，不产生代码）。删除需
   单独一次改动 + 单独的全量门禁，并显式声明「本项是删除而不是迁移」。
2. `War3SemanticBuildWorldPaletteIfNeeded`（A9）：**建议迁移而非删除**，理由：它虽然当前
   0 调用，但它描述了「model-local palette → world-space 组合」这条**已被 A4 使用的合同**
   的完整写法（`outPalette = worldTransform * localMatrix`），在 device.cpp 里是一份
   v1.27a 语义参考。稳妥处置是与 A4 一起放进模块（正文逐字节、配 legacy 覆盖），
   或显式标注为「保留的参考实现」并加入 FROZEN（baseline=0、budget=1）；两条都需要
   单独一次改动，避免把「死代码处置」夹带进结构性迁移。
3. **不要**在本轮把二者之一删除或迁移：清册 §1.4 第 5 条与本工单都要求「先报告裁定建议」。

### 10.2 其它未覆盖项

1. **device.cpp 调用点编排（M2-5 表 B 的 B1/B2/B4/B5/B6–B11）不在本轮范围**：live palette
   refresh 块、paletteSource/slotIndex 装配、`War3TryBuildLiveRuntimeGroupPalette` 提交点、
   `War3NoteLivePaletteMotion` 调用点、taxonomy 探针表等一个字节没改，也**没有**被本电池覆盖。
2. **调用点文本是新增文本**：A1/A2/A8 的 device.cpp 调用点**未改**（仍是原行），但 A4/A5 的
   调用点之所以也「未改」，靠的是 using-directive 解析；差分不覆盖「device.cpp 取到的实参」
   —— 这只能由代码审阅 + 全量构建 + 后续实机门覆盖。
3. **A8 的 6 个 bounds 调用点未被显式驱动**：它们只是解析到模块定义（纯 switch 查表，
   无状态），由 device.cpp 全量编译与既有门禁覆盖；本轮没有为 bounds 族新增差分。
4. **A8 的职责归位是工程裁定**：它被放进 palette 模块（理由见 §1.2 第 3 点），不是
   「bounds 族已迁完」。若将来有独立 bounds 模块，可再把定义移过去（正文逐字节、
   6 个调用点仍不需改）。
5. **`IsReadableRange` 在宿主机是替身**：差分/probe 证明「同一输入 + 同一替身世界下两个
   实现一致」，**不证明**生产内存原语本身（A3/A4 的可读性判定在实机上依赖真实
   `VirtualQuery` 路径）。
6. **env 覆盖为 4 个矩阵 + probe 的 2 组**而非穷举：env 取值形态（0/1/未设置/十六进制/
   非数字/空串）由 M2-1 的 `--probe` 表承担；本电池只覆盖 A1/A2 的 on/off 组合。
7. **单线程**：A1/A2 的 `static const bool` 与 A4 无跨帧状态；本电池不做多线程差分
   （M2-4 迁出的函数**没有** `thread_local` / 跨帧表，这是与 M2-5 的实质差别）。
8. **变异 B 只证明门禁的点名能力**：那段回流代码没有调用者，因此不覆盖「回流后的运行行为」；
   它证明的是「可编译回流也不会漏过判定 1（+ 本轮 M2-4 门禁）」。
9. **`--probe-m2-4` 的期望值表**是「先手工推导、再与实跑对照修正」得到的回归 pin，
   不是独立第三方来源；它的价值是钉死绝对语义与发现「两侧同步漂移」，不是数学证明。
10. **本测试不覆盖 GPU / Vulkan / 实机画面 / 性能**；未部署、未启动游戏、未做玩家前台验收。
    本轮全部证据为宿主机差分 + 静态门禁，**不构成稳定版依据**，也不得表述为
    「palette 侧职责已迁完」（M2-5 的 B1/B2/B4/B5/B6–B11 与 A9/A10 仍未做）。

## 11. 与其它文档的边界

- 本文**不改**`2026-09-18-m2-5-taxonomy-and-remaining-scope.md` 的任何结论；它是设计与清册。
- 本文**不改** M1/M2-1/M2-2/M2-3/M2-5 的等价记录、五份既有 `.inc` 与既有门禁的语义
  （只按先例改锚 `test_direct_geoset_owner_handoff_static.py` 一处，见 §8）。
- 本文**不是**「M2-5 已完成」「palette 侧职责已迁完」或稳定版依据：只完成了 M2-4 的 8 个符号。
- 本轮**未** git 写、未部署、未启动游戏、未称稳定版。

## 12. 文档写入后的复跑

本记录 §1–§11 与进度日志 / 开发台账写入完成后再跑一次预算门禁、M1/M2/M2-3/M2-5/M2-4
等价门禁与 `ninja -n`，要求全部 exit 0（M2-3/M2-5 同型）。结果见本节末尾的复跑记录。

- 复跑窗口（机器本地时钟）：见 `docs/plan/2026-09-18-m2-4-full-gate-rerun.log` 头部的
  `date:` 行。**本次复跑 = §9 那张表本身**：记录主体（§1–§11）、进度日志与开发台账
  均在本次复跑**之前**写入，复跑在同一棵树上重跑全部 21 个门禁段。
- 结果：`ninja -n` no work（exit 0）→ touch 后真实重建 `NINJA_EXIT=0`（日志 `[1/6]`–`[6/6]`）
  → 预算门禁 exit 0（冻结 155 / 站点 165→112→112 / 已迁出 46 / 568）→ M1 exit 0 → M2 exit 0
  → M2-3 exit 0（`motion-on 差分 37920642`）→ M2-5 exit 0（`diagnostics-on 差分 37920642`）
  → **M2-4 exit 0** → meson `Ok: 83 Fail: 0` → 全量静态 `STATIC_TOTAL=252 STATIC_PASS=252
  STATIC_FAIL=0` → 记录器 23/23 → 成本 PASS(38/38) → 生命周期 187/187 → 解析器 `Ran 94 tests OK`
  → 根读方 `Ran 55 tests OK` → 往返 `CHECKS=1107 FAILURES=0` + `CERTIFIED_SPEC_SATISFIED`
  → 差分四矩阵 EXIT=0 → `--probe-m2-4` 两组 env EXIT=0 → `ninja -n` no work。
- 复跑产出 DLL `36277956 B / BDB47E4E238CE9D794EA251EF533F6EA6CA29F833D77214D318E989340396851`。
- 诚实标注（顺序）：§9/§9.3 的 DLL 身份与本节数字是复跑后**回填**的；除这两处数字外，
  记录主体在复跑前已定稿，本轮未再改动任何源码或门禁。
