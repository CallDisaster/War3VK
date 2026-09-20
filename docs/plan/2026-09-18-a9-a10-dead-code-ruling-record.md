# A9 / A10 死代码裁定落地记录：A10 删除、A9 迁出到调色板模块

> 状态：**A10 与 A9 两次独立改动均已落地**（A10 = 删除未实例化 function
> template；A9 = 逐字节迁出到既有调色板模块 + legacy 参考 + 宿主差分 + 独立门禁 +
> FROZEN_A9）。两次改动**各自的** provenance / 快照 / 变异真跑 / 门禁证据分开记录
> （§1–§6）。全量门禁在**最终树**取数（§8），原始输出见
> docs/plan/2026-09-18-a9-a10-full-gate-rerun.log。
> 本轮**不部署、不启动游戏、不 git 写、不触碰 E:\Work 下任何文件、不称稳定版**。

工单来源：主线程 2026-09-18 裁定（承接
docs/plan/2026-09-18-m2-4-migration-equivalence-record.md §10.1 的裁定建议与
docs/plan/2026-09-18-m2-5-remainder-record.md 的 A9/A10 行）：
- **A10 War3SemanticVectorStorageReadable（未实例化 function template）⇒ 删除**
  （须同步改 M2-4 门禁 DEAD_CODE_STILL_IN_DEVICE 断言）；
- **A9 War3SemanticBuildWorldPaletteIfNeeded（[[maybe_unused]]，0 调用）⇒ 迁出到
  已有调色板模块**（其调用链的 A4 已在该模块），正文逐字节随迁；名字不匹配语义命名族
  ⇒ 同一次改动登记 FROZEN 增量项（baseline 用预算门禁同一 DEF_RE 实测，budget=0）。

## 0. 结论摘要

- **A10**：src/d3d9/d3d9_device.cpp 删掉 11 行（template 头 + 定义 + 尾随空行），
  文件 **2,249,192 B → 2,248,823 B（-369 B）**。删除仅限该模板本身：同一改动只另有
  M2-4 门禁的死代码断言反向（其余一个字节未动，由 §1.1 的还原证明给出）。
- **A9**：War3SemanticBuildWorldPaletteIfNeeded 的函数**正文逐字节**迁入
  src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp} 的 A9 BEGIN/END 标记之间；
  device.cpp 只减少 15 行（定义 + 尾随空行）**0 行替换文本**（A9 在 device.cpp 没有
  调用点，因此没有锚注释，见 §1.2）。device.cpp **2,248,823 B → 2,248,277 B（-546 B）**。
- **A9 legacy 参考**：新文件
  src/d3d9/war3/semantic/tests/war3_live_palette_selection_a9_legacy_reference.inc
  = **3,026 B / 57 行**，
  D92048FCB8B293C2A738051043C918240BBCD5759459FA029744D8FADE3FD66F，由入库生成器
  AutoTest/gen_war3_live_palette_selection_legacy_reference.py --a9 产出；
  **前六份 .inc 与其 SHA 一字未动**。
- **双向 fail-closed 三方一致**：模块正文 == .inc 正文 == pre-A9 快照独立重抽正文
  （生成器与独立门禁**各自**复算；模块侧签名去掉 [[maybe_unused]] 与 device 侧签名
  不同，但**正文逐字节相同**）。
- **宿主差分 N/N**：A9 电池 **947,458** 条断言（236,864 次调用 x 4 条 + 2 次段末
  memcmp），4 组 env 矩阵全部 passed == total、0 DIFF；--probe-a9 17 场景 x 2 组
  env，逐行 module == legacy == 门禁内独立推导（含 0 期望与 256 元素输出）。
- **两条变异真跑**（§6）：A10 用"把该模板重新加回 device.cpp" ⇒ M2-4 门禁**点名**该
  模板文本（预算门禁对此**无能力**，仍 exit 0）；A9 用模块侧正文语义漂移
  （worldTransform * localMatrix → localMatrix）⇒ 差分红 + A9 门禁红。两条都完成
  「改 → 构建 → 跑 → 红 → 还原 → touch → 真实重编译 → 复绿」并附 SHA MATCH 证明。
- **命名族盲区**：A9 的名字不匹配预算门禁 SEMANTIC_FAMILY_RE（无
  Resolve/Select/Runtime/Key/... 后缀），且其定义行以 [[maybe_unused]] 开头不占
  DEF_RE 行首站点（实测 baseline = **0**）⇒ 已登记 FROZEN_A9 = {A9: (0, 0)}，
  使"去掉属性后回流"在判定 1 被点名。A10 是**删除而不是迁移**，因此**不得**登记进
  FROZEN（判定 2 会把"站点 0 < baseline"读成"没落地的删除/改名"）；A10 的捕手是
  M2-4 门禁的文本缺席断言（§5）。
- **未覆盖项**见 §9：A9 在生产代码里**仍然 0 调用**（迁移保持"死代码但可审计"），
  本轮不证明它被使用；不覆盖 GPU/实机；不称"palette 侧职责已迁完"。

## 1. 两次独立改动与字节账

### 1.1 A10：删除未实例化 function template（独立改动 1）

| 项 | 值 |
| --- | --- |
| 被删文本 | 11 行（template <typename T> / bool War3SemanticVectorStorageReadable(...) 签名 2 行 / 正文 7 行 / 右花括号 / 尾随空行），**仅此一块** |
| src/d3d9/d3d9_device.cpp pre-A10 | **2,249,192 B** / AC5FB37E3BFE65BA8DA47E173C354EF8E405E4EEA95A1CB417212020EA80E380 |
| src/d3d9/d3d9_device.cpp post-A10 | **2,248,823 B** / FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C |
| delta | **-369 B**（= 被删块的 CRLF 字节数，脚本实测 len(block) == 369） |
| 其它文件 | 仅 AutoTest/test_war3_palette_m2_4_equivalence_static.py 29,033 → 30,028 B（死代码断言反向 + 文档字符串同步） |

**删除的精确性证明**：A10 的写入源是 %TEMP%/device_pre_a10.cpp 快照本身
（脚本 fail-closed 校验 SHA/字节数后，只做一次 replace(block, "", 1)），并且
变异阶段把同一 block 原样插回后**逐字节复现了 pre-A10 快照 SHA**
（MUTATED equals pre-A10 snapshot: True，§6.1）⇒ 反向证明该改动确实只删了这一块。

**裁定语义（不得混淆）**：A10 是**删除**，不是迁移。它不是 FROZEN 项：把它登记进
FROZEN 会让预算门禁判定 2（"站点数低于 baseline 的符号必须出现在 MODULE_HOME"）
把它读成"改名绕过 / 直接删除"。删除后 War3SemanticVectorStorageReadable 在 device.cpp
出现 **0** 次（M2-4 门禁的 DEAD_CODE_ABSENT_FROM_DEVICE 断言 + 本轮实测）。
模板未实例化 ⇒ 不产生任何代码；它在 device.cpp 里曾贡献 **1** 个 DEF_RE 行首站点
（bool War3SemanticVectorStorageReadable(const std::vector<T>& values, 行），因此预算
门禁的"device.cpp 行首定义站点总数"从 **568 → 567**（判定 1/2/3 均不涉及该符号）。

### 1.2 A9：正文逐字节迁出到调色板模块（独立改动 2）

| 文件 | pre-A9 | post-A9 | delta |
| --- | --- | --- | --- |
| src/d3d9/d3d9_device.cpp | 2,248,823 B / FB4D2FF6..716C | **2,248,277 B** / 4E82651458826BBAB4BE3536893AADDFACF253C5694849F9528309133C2165FE | **-546 B** |
| src/d3d9/war3/semantic/war3_live_palette_selection.cpp | 48,248 B / 89D506FE..E6F5 | **49,791 B** / D019FD7DD68CC46A5C3D68AE72270713FCC5B7693A058A8224BBBDEA53F47225 | +1,543 B |
| war3_live_palette_selection.h | 12,358 B / CAF54E2A..23C9 | **13,286 B** / DB22C74D2AF4854B103CD2AAF544AAB9CA5B05A07E0DB6A650D6EE2748C21EB2 | +928 B |
| tests/war3_live_palette_selection_a9_legacy_reference.inc | - | **3,026 B** / D92048FCB8B293C2A738051043C918240BBCD5759459FA029744D8FADE3FD66F | 新增（生成器 --a9） |
| tests/war3_live_palette_selection_test.cpp | 168,483 B / 2B8C2ECB..CE03 | **177,487 B** / 32CA207AA1EA4CFFE5F360199D8A741ECCF09B20DCD35A0AD946D1654587D61C | +9,004 B |
| AutoTest/gen_war3_live_palette_selection_legacy_reference.py | 55,863 B / B5CC80C0..6142 | **64,287 B** / 69097872DC29D372DD3971F16B59EFACDF83D25B70606DAA2DB62B5026A9008A | +8,424 B（--a9 模式） |
| AutoTest/test_device_semantic_responsibility_budget_static.py | 24,341 B / 78C7C14A..D631 | **25,945 B** / D5CEC3E34F34FBBAE928B5768EBAAE1008B74C461E19A29DDABDF58D9D2AC559 | +1,604 B（FROZEN_A9） |
| AutoTest/test_war3_palette_m2_4_equivalence_static.py | 30,028 B / 1E3BD7C9..1CF0 | **30,184 B** / 7E16CD340972EDE58A8DF735FCEE72D37F5F42B94ABEF0FF7FE4F55676CB0E3E | +156 B（A9 断言反向） |
| AutoTest/test_war3_palette_a9_migration_equivalence_static.py | - | **28,632 B** / AA018C2087670A60500864DEED9CE7E23DEE73A86B9CF8CF4A2673A48BAB24A3 | 新增（全量静态 253 → 254） |

**device.cpp -546 B 的构成**：A9 定义的 15 行（14 行内容 + 1 行尾随空行）CRLF 字节数
= 546。**没有**插入锚注释：与 M2-4 的 8 个符号不同，A9 在 device.cpp **没有调用点**，
因此按"正文逐字节随迁"的最低改动原则只做删除；迁移事实由模块侧 BEGIN/END 标记、
生成器 --a9 与独立门禁记录（不发生"注释锚"缺口）。

**唯一机械差异（显式声明）**：device 侧签名是 [[maybe_unused]] void
War3SemanticBuildWorldPaletteIfNeeded(，模块侧是 void
War3SemanticBuildWorldPaletteIfNeeded( —— **签名层**去掉 [[maybe_unused]]
（匿名命名空间才需要该属性；模块内定义有外部链接，未使用不触发
-Wunused-function）。**正文层一个 token 未改**，由生成器与门禁各自逐字节复算。

**落点理由**：A9 的正文只调用 War3SemanticPaletteLooksModelLocal（A4 compose-policy）
与 std::vector/Matrix4 运算；A4 及其 supporting（A3/A6/A7/A8）已在
war3_live_palette_selection.{h,cpp}，因此 A9 属同一单一职责（palette compose），
按 M2-4 §1.2 的同一落点规则迁入既有模块，**不新开模块**。迁移保持"逐字节等价"，
不改语义、不加 env、不新增调用者。

**A9 迁移后 device.cpp 的 A9 出现次数 = 0**（[[maybe_unused]] void <name> 与
DEF_RE 行首站点均为 0，见 §5 门禁复算）；模块侧 .cpp 定义恰好 1 处、.h 声明恰好
1 处（带 outPalette 形参、不带 [[maybe_unused]]）。

## 2. provenance（fail-closed）

两次改动**各自独立**捕获（机器本地时钟；工作树文件哈希，**不是** git blob）：

| 步骤 | 文件 | 字节数 | SHA-256 | 捕获时间 | %TEMP% 备份 |
| --- | --- | --- | --- | --- | --- |
| pre-A10 | src/d3d9/d3d9_device.cpp | 2,249,192 | AC5FB37E3BFE65BA8DA47E173C354EF8E405E4EEA95A1CB417212020EA80E380 | 2026-09-18T04:44:58 | device_pre_a10.cpp |
| post-A10 | 同上 | 2,248,823 | FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C | - | device_post_a10.cpp |
| pre-A9 | 同上（= post-A10，交叉验证闭合） | 2,248,823 | FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C | 2026-09-18T04:49:48 | device_pre_a9.cpp |
| post-A9 | 同上 | 2,248,277 | 4E82651458826BBAB4BE3536893AADDFACF253C5694849F9528309133C2165FE | - | D:\tmp\a9_a10_work\ 脚本可重放 |

- pre-A10 / post-A10 / pre-A9 三次捕获的其余文件（模块 .h/.cpp、差分测试、生成器、
  预算门禁、M2-4 门禁、src/d3d9/meson.build）分别登记在
  %TEMP%/a9_a10_provenance_pre_a10.txt 与 %TEMP%/a9_a10_provenance_pre_a9.txt；
  两份清单与本文 §1.1/§1.2 的表逐字节一致。
- 一次性改动脚本（不入库，审计用）：D:\tmp\a9_a10_work\apply_a10.py、
  apply_a9_code.py、apply_a9_tests.py、mutate_a10.py、mutate_a9.py、ninja.cmd
  （Below Normal + -j2）与日志 ninja.log。
- **A9 迁移前的定义行号以 pre-A9 快照为准 = :7161-7175**（pre-A10 时是 :7172-7186；
  A10 删掉上方 11 行后整体上移）。生成器常量 A9_LINES 与门禁登记同值，写错即红。

## 3. A9 legacy 参考与双向 fail-closed（--a9）

- 新文件 tests/war3_live_palette_selection_a9_legacy_reference.inc = **3,026 B / 57 行**，
  D92048FCB8B293C2A738051043C918240BBCD5759459FA029744D8FADE3FD66F。
- 生成器 --a9 的 fail-closed 三步（与 --m2-4/--m2-5b 同型）：
  1. 快照 SHA-256 / 字节数必须等于登记常量（A9_EXPECTED_SHA256 / A9_EXPECTED_SIZE）；
  2. 定义锚唯一命中、行号等于登记范围（A9_LINES）、正文不得出现 m_war3Scene /
     m_war3Semantic / m_state / VisibleRenderableRegistry::instance / this->，且正文
     必须仍含 War3SemanticPaletteLooksModelLocal(（A4 调用链合同）；
  3. 模块 .cpp 的 A9 BEGIN/END 之间正文必须与快照抽取结果**逐字节相同**（实测通过）。
- .inc 的机械改写规则与 M2-4 相同：外层命名空间改为 m2_legacy_reference、行尾 LF
  归一；dxvk::war3::ObjectKind / dxvk::war3::IsReadableRange 是两侧共享的真实契约
  原语，刻意不改写。**关键顺序**：.inc 必须 include 在 M2-4 .inc **之后** —— 正文里
  无限定的 War3SemanticPaletteLooksModelLocal(...) 因此解析到 M2-4 的 legacy A4
  副本（A4 → A3/A6/A7/A8 同命名空间 legacy 副本），与 pre-A9 device.cpp 经
  using-directive 的真实调用目标一致。**门禁静态检查点**：A9 include 的下标必须大于
  M2-4 include（顺序写错即红）。
- 三方独立复算：生成器（§3 第 3 步）、A9 门禁（自己抽 .inc 与模块两侧正文比较）、
  A9 门禁在快照在位时**再从快照独立重抽**（锚唯一 + 行号 + 禁 token + A4 调用）并与
  .inc 正文比较。三者任一漂移即红。

## 4. 宿主差分（A9 电池 + --probe-a9）

被测实现 = 两侧的 War3SemanticBuildWorldPaletteIfNeeded(palette, world, kind, out)；
输出面 = outPalette 的元素个数与**逐字节内容**（每个 Matrix4 的 16 个 float 位模式，
顺序敏感）。每个用例 4 条断言：整个 A9Result 的 memcmp、count、hash、输出 vector
逐字节 memcmp；每段结束再对两侧累计结构做一次 memcmp。**容差 = 0**。

| 电池 | 规模 | 覆盖 |
| --- | --- | --- |
| 系统网格 | **36,864 次调用** | 9 palette variant（空 / 1 / 4 / NaN / 1e6 / Inf / 500-700 / **256** / **257**）x 2 可读性注册 ON/OFF x 8 world（0 / 4.0001（worldMagSq 16.0008）/ 4.0（恰好 16）/ 1000 / 4096 / NaN / Inf / 5.0）x 256 kind（全 uint8） |
| 定种子随机 | **200,000 次调用** | splitmix64 随机矩阵位模式（含特殊位）、随机 world（1/4 取 M24World 固定变体）、size = roll % 13 的 vector 长度、1/64 概率注入 **257** 项宽 palette、随机 kind/可读性注册 |

合计 **236,864 次两侧调用**；每次 4 条（结构 memcmp + count + hash + 内容 memcmp）
⇒ **947,456**，加 2 次段末累计 memcmp ⇒ **A9 电池 947,458 条断言**（测试自身打印，
门禁按**精确值 + 下限**双重咬合）。实测（最终树）：4 组 env 矩阵
"A9 battery: 947458 checks, 0 failures"，全文件 40215909/40215909 checks passed
（default；palette-diag 40215908，contract 36727797，contract-diag 36727796）。
模块正文与 legacy 正文逐字节相同的长度是 **321 B**（A9 函数正文，门禁与生成器各自复算）。

--probe-a9 = **17 个固定场景 x 2 组 env**（default /
DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1），逐行要求 module == legacy，并与门禁内
**独立实现**的 A4/A9 语义推导（§5 的 expected_a9）逐字段相等：覆盖"model-local 成立
→ 逐元素 world * local 组合"、storage 可读性失败、size 257 越界、空 palette、
palette NaN/Inf、world NaN/Inf/0/恰好 16/16.0008、kind 半径分档（0/1/2）、以及
256 元素输出的完整 FNV 值。期望值先在门禁内独立推导、再与实跑对照后固定；门禁每次
运行都执行 self_check_expectations()，独立推导与登记值不一致即红（防止"两侧同步漂移"）。

## 5. 独立 A9 等价门禁

新文件 AutoTest/test_war3_palette_a9_migration_equivalence_static.py（全量静态
**253 → 254**）。静态 fail-closed：

1. 证据文件齐全：legacy .inc、入库生成器、预算门禁、模块 .h/.cpp、差分测试、meson、
   本记录文档；
2. legacy 参考 provenance（pre-A9 SHA-256 / 字节数 / 捕获时间 / worktree-vs-git-blob
   说明 / 生成器名）与**文件自身** SHA-256、字节数 == 登记值（故意改正文即红）；
3. 正文逐字节：门禁**自己**抽 .inc 的 A9 定义正文，与模块 .cpp A9 BEGIN/END 之间
   正文比较（不依赖生成器自证）；
4. 模块 .h 有 A9 声明（含 outPalette 形参、不带 [[maybe_unused]]）；模块 .cpp
   定义也不带该属性、模块侧签名恰好 1 处；
5. device.cpp：A9 定义文本 / 定义行 / DEF_RE 行首站点**都必须为 0**（用预算门禁同一
   DEF_RE 与同一"第 0 列、非注释行"规则在门禁内 eval 复算，不抄写）；
6. FROZEN_A9 键集合 == COVERED、值为 (0, 0)、且有 FROZEN.update(FROZEN_A9)；
   **原始 FROZEN 块里不得出现 A9**（否则 M1 等价门禁会把它误判为 M1 迁出）；
7. 生成器含 --a9 / main_a9 / A9_EXPECTED_SHA256 / A9_LINES / A9_DEV_SIG /
   A9_MOD_SIG / A9_END / A9_OUT_PATH；
8. 差分测试 include 了 A9 .inc 且**位置在 M2-4 .inc 之后**、有 sem::/legacy::
   两侧调用点与 A9 电池/探针原语；
9. meson 的 war3_live_palette_selection_test 目标仍链接真实模块 .cpp 与差分测试；
10. %TEMP%/device_pre_a9.cpp 在位时：验证快照身份 → 独立重抽 A9 定义（锚/行号/
    禁 token/A4 调用）与 .inc 正文比对 → 用**同一个** DEF_RE 实测 baseline=0
    （快照不在位时打印说明，但其余静态项仍 fail-closed）。

动态：4 组 env 矩阵 exit 0、passed == total、总检查数 >= 34,000,000，且 A9 电池
**精确** 947,458 + 0 failures；--probe-a9 2 组 env 逐行对照独立期望值表。

**A10 的捕手（为什么它不进 FROZEN）**：A10 已删除，M2-4 门禁的
DEAD_CODE_ABSENT_FROM_DEVICE 断言钉住"该模板定义文本不得再出现"；预算门禁的三条
判定都不会咬它（命名族不匹配 + 删除后无站点）。这是本轮显式登记的能力边界。

## 6. 变异真跑（2 条）

### 6.1 变异 A10：把删掉的模板重新加回 device.cpp（死代码门禁必须点名）

- 改：把 §1.1 的 11 行 block 原样插回 "// M2-4: War3SemanticPaletteStorageReadable"
  注释之后。变异后文件 **2,249,192 B /
  AC5FB37E3BFE65BA8DA47E173C354EF8E405E4EEA95A1CB417212020EA80E380**
  —— **逐字节等于 pre-A10 快照**（脚本自报 MUTATED equals pre-A10 snapshot: True）。
- 构建：NINJA_EXIT=0，日志 [1/2] Compiling C++ object
  src/d3d9/d3d9.dll.p/d3d9_device.cpp.obj / [2/2] Linking target src/d3d9/d3d9.dll
  （**能编译** —— 正是"删错了会静默回来"的回归类型）。
- 跑（红）：M2-4 门禁 **EXIT 1**：AssertionError: device.cpp 里的 A10 未实例化模板
  仍然存在（删除未落地/回流）：bool War3SemanticVectorStorageReadable(const
  std::vector<T>& values,
- **同时实测的能力边界**：预算门禁在该变异下仍 **EXIT 0**
  （冻结符号 156；... device.cpp 行首定义站点总数 568）⇒ 该模板的删除/回流**只能**由
  M2-4 门禁文本断言捕获。
- 还原：%TEMP%/device_post_a10.cpp 复制回 + LastWriteTime 置当前时刻 ⇒
  RESTORED_SHA=FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C、
  RESTORED_MATCH=True；真实重编译（[1/2] 编译 + [2/2] 链接）后 M2-4 门禁 **EXIT 0**。

### 6.2 变异 A9：模块侧正文语义漂移（差分必须红）

- 改：模块 .cpp 的 A9 正文把 `outPalette.push_back(worldTransform * localMatrix);`
  改成 `outPalette.push_back(localMatrix);`（**语义漂移但可编译**：丢掉 world 组合）。
  变异后模块 .cpp = **49,774 B /
  4EB434C3EC286F84EBEB3907887F4FA3EFEFFB197CBF9F88DB02C6D56A66D5BE**。
- 构建：NINJA_EXIT=0，日志 `[1/4]`–`[4/4]` 真实编译 + 链接两个目标
  （war3_live_palette_selection_test.exe 与 d3d9.dll）⇒ **可编译漂移**。
- 跑（红）：
  - 差分测试 **EXIT=1**：`A9 battery: 947458 checks, 26672 failures`，
    全文件 `40189237/40215909 checks passed`；stderr **26,672** 条 DIFF，首 3 条：
    `DIFF line 4058 A9Grid: A9Result memcmp mismatch` /
    `DIFF line 4058 A9.hash: module=14971461764820984933 legacy=6770109457156564069` /
    `DIFF line 4058 A9Grid: A9 outPalette bytes mismatch`。
    **M2-4 / M2-5B 电池仍各 0 failures** —— 差异被准确定位在 A9 段。
  - A9 门禁 **EXIT=1**：`模块 .cpp 的 A9 BEGIN/END 之间正文与 legacy .inc 正文逐字节
    不一致（迁移文本已漂移）`。
  - **预算门禁仍 EXIT 0**（`冻结符号 157；... device.cpp 行首定义站点总数 567`）⇒
    值级漂移只能靠差分/A9 门禁咬住（与 A10 变异同型的能力边界）。
- 还原：%TEMP%/module_post_a9_live_palette_selection.cpp 复制回 + LastWriteTime 置当前时刻
  ⇒ `RESTORED_SHA=D019FD7DD68CC46A5C3D68AE72270713FCC5B7693A058A8224BBBDEA53F47225`、
  `RESTORED_MATCH=True`；真实重编译（`[1/4]`–`[4/4]`）后差分
  `A9 battery: 947458 checks, 0 failures`、`40215909/40215909 checks passed`、
  A9 门禁 **EXIT 0**。

## 7. 跟随改动改锚的既有门禁（断言语义未削弱）

M2-4 等价门禁 AutoTest/test_war3_palette_m2_4_equivalence_static.py 原先钉住
"A9 与 A10 **仍在 device.cpp**"。两次改动各改一次该断言（A10 改一次、A9 再改一次），
最终形态是一条**反方向**断言：DEAD_CODE_ABSENT_FROM_DEVICE 中的两条文本都不得出现
在 device.cpp，并点名"A9 的等价证据在独立门禁内"。**其它断言一条未改**：8 个 M2-4
符号的行首站点 0、9 条迁移锚注释、FROZEN_M2_4 键集合/baseline/原始 FROZEN 块里 A8
仍为 (1, 1)、FNV 自证、meson/生成器 CLI、差分测试原语、4 组 env 矩阵与
"M2-4 battery" 精确值（7,477,257）。A9 的正文等价证据被独立门禁接管（并更严：加了
逐字节三方复算与探针期望值表）。

## 8. 全量门禁（最终树）

窗口：两条变异均已还原、源码 SHA 与 §1.2/§6 一致。原始输出
docs/plan/2026-09-18-a9-a10-full-gate-rerun.log（本次复跑 = 本节这张表）。

| # | 门禁 | 原始结果 |
| --- | --- | --- |
| 0 | 源码身份 | 见 §1.2 与日志 §0（11 个文件逐个字节数 + SHA-256） |
| 1 | ninja -C build32 -n | ninja: no work to do.，EXIT=0 |
| 2 | 最终树真实重建（Below Normal + -j2，touch 三个源后） | NINJA_EXIT=0；日志 [1/6] 差分测试模块、[2/6] 差分测试本体、[3/6] DLL 模块、[4/6] Linking war3_live_palette_selection_test.exe、[5/6] Compiling d3d9_device.cpp.obj、[6/6] Linking target src/d3d9/d3d9.dll |
| 3 | 预算门禁 | EXIT 0：冻结符号 **157**；行首站点 迁移前 165 -> 迁出后上限 112 -> 当前 **112**；已迁出 **46** 个符号；device.cpp 行首定义站点总数 **567**（A10 删除模板后 568 -> 567） |
| 4 | M1 等价门禁（**一个字节不改**） | EXIT 0：已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / 2EA43F9782BE388E…；下限 3,000,000；probe 6 组 |
| 5 | M2 等价门禁（**一个字节不改**） | EXIT 0：M2-1 8 + M2-2 链 1 全部有覆盖；M2-1 ref 7702 B / D458C1AE…；链 ref 36025 B / 02FF8AFE…；下限 2,000,000（各矩阵另有链下限）；probe 6 组 + probe-chain 3 组 |
| 6 | M2-3 等价门禁（**一个字节不改**） | EXIT 0：motion 3 符号 + 2 Entry；ref 8406 B / 7DF61688…；**motion-on 差分 40215908**（下限 12,000,000） |
| 7 | M2-5 等价门禁（**一个字节不改**） | EXIT 0：34 个字段单一实现；ref 22511 B / A5333AC7…；正文逐字节 18937 B；**diagnostics-on 差分 40215908**（下限 21,000,000） |
| 8 | M2-4 等价门禁（本改动只改死代码断言） | EXIT 0：8 符号 / 9 定义；ref 9397 B / E9CB8931…；正文逐字节（门禁独立复算）；probe 20 × 2；M2-4 电池下限 7,400,000（登记精确值 **7,477,257**） |
| 9 | M2-5B 等价门禁（**一个字节不改**） | EXIT 0：ref 4952 B / C6D32B4E…；正文逐字节 2421 B；快照独立重抽一致；probe 12/12 × 2；M2-5B 电池 **1,347,808** |
| 10 | **A9 等价门禁（本轮新增）** | EXIT 0：A9 1 符号；ref 3026 B / D92048FC…；正文逐字节 **321 B**；pre-A9 device.cpp 2248823 B / FB4D2FF6…；**快照独立重抽一致 + DEF_RE baseline=0**；probe-a9 **17/17 × 2 组 env**；A9 电池 **947,458**（下限 940,000）；总检查 default **40215909** / palette-diag 40215908 / contract 36727797 / contract-diag 36727796（下限 34,000,000） |
| 11 | meson test -C build32 --num-processes 2 | Ok: **83**  Fail: **0**，EXIT=0（**83→83**：A9 的电池/探针加在既有差分测试目标内，未新增 meson 目标） |
| 12 | 全量静态 AutoTest/test_*_static.py 逐个 | **STATIC_TOTAL=254 STATIC_PASS=254 STATIC_FAIL=0**（**253→254**：新增 A9 门禁 1 个） |
| 13 | 记录器 war3_palette_object_evidence_test | SUMMARY: **23 passed, 0 failed**，EXIT=0 |
| 14 | 成本 war3_palette_object_evidence_cost_test | COST_VERDICT=**PASS checks=38 failures=0**，EXIT=0 |
| 15 | 生命周期 war3_shadow_build_lifecycle_test | **187/187** case(s) passed，EXIT=0 |
| 16 | 解析器静态 test_palette_object_evidence_analysis_static.py | Ran **94** tests … OK，EXIT=0 |
| 17 | 根读方 test_analyze_frame_evidence.py | Ran **55** tests … OK，EXIT=0 |
| 18 | 往返 test_palette_object_wire_roundtrip.py | CHECKS=**1107** FAILURES=**0**；ROUNDTRIP_VERDICT=**CERTIFIED_SPEC_SATISFIED**（A–F），EXIT=0 |
| 19 | 差分四矩阵（A9 门禁内 + 各门禁自带矩阵） | 全 EXIT=0；default **40,215,909/40,215,909**、palette-diag 40,215,908、contract 36,727,797、contract-diag 36,727,796 |
| 20 | DLL 身份 | **36,278,629 B** / 824AA63B5C151831F19F7599BF89ABFFE92967564A38236B95056009C01D1887（§8.2） |
| 21 | 复跑 ninja -C build32 -n | ninja: no work to do.，EXIT=0 |

### 8.1 需要点名的新事实（与 M2-5B 同类）

M2-3 门禁打印的 motion-on 差分数与 M2-5 门禁打印的 diagnostics-on 差分数**都是全文件
累计检查数**：M2-5B 之后是 39,268,450，本轮 A9 电池把它抬到 **40,215,908**。两条门禁
的登记下限（12,000,000 / 21,000,000）仍满足，M2-3/M2-5 的语义覆盖不受影响，但这两个
数字**不再等价于**各自的专项覆盖量（门禁本身未改）。同理 M1/M2/M2-4/M2-5B 门禁打印的
总检查数也随 A9 电池上移，它们各自登记的是下限/电池精确值，不受影响。

### 8.2 DLL 身份与可复现性

最终产物 build32/src/d3d9/d3d9.dll = **36,278,629 B**，
SHA-256 **824AA63B5C151831F19F7599BF89ABFFE92967564A38236B95056009C01D1887**
（最终树最后一次真实构建：touch 三个源后日志出现 [1/6]–[5/6] 真实编译与
[6/6] Linking target src/d3d9/d3d9.dll）；差分测试 exe = 6,554,468 B /
2B984E0E567F9045BE868A72FC00F946692FE7CBBB9728BC1584B5854E8D8054。
对照 M2-5B 的最终 DLL（36,278,340 B / 7DA555A7…）⇒ 尺寸 **+289 B**（device.cpp 净减
915 B 源码、模块 .cpp 净增 1,543 B 源码，另加真实重链的链接时间戳）。

**同尺寸不同 SHA（沿用既有实测）**：同一最终源码在多次链接后得到**同尺寸、不同 SHA**
的 DLL（MinGW ld 写入链接时间戳）。本记录登记的是最终树**最后一次真实构建**的身份；
本轮中间态也实测到 36,278,629 B / 30DA1FC858A8CBBE… 的同尺寸不同 SHA 实例。
**未部署**：本轮没有覆盖任何游戏目录文件，未启动游戏或编辑器，E:\Work 下未触碰
任何文件。

## 9. 未覆盖项（如实声明）

1. **A9 在生产代码里仍然 0 调用**：裁定是"迁移而非删除"，迁移后它依旧没有任何
   调用者（这也是它当初被判为死代码的原因）。本门禁证明的是"迁移逐字节等价 + 合同
   语义一致 + 可审计"，**不是**"A9 被使用了"。
2. **宿主差分是替身世界**：IsReadableRange 在宿主机是替身（§4），因此不证明生产
   内存原语本身；--probe-a9 的期望值表是"先手工推导 + 独立实现复算 + 与实跑对照"
   的回归 pin，不是独立第三方来源。
3. **A10 只删模板本身**：本轮没有顺手删除任何其它符号/注释；A10 的删除不会改变编译
   产物（模板 0 实例化），因此"产物不变"不是证据，文本缺席 + 门禁点名才是。
4. **A9 的 device.cpp 侧没有锚注释**：与 M2-4 的 9 个锚不同，A9 无调用点，留注释会
   制造"仍在使用"的错觉；迁移事实由模块标记 + 生成器 + 独立门禁记录。若将来需要
   device.cpp 侧的迁移可见性，应另开改动，不在本轮。
5. **A9 不覆盖 device.cpp 调用点编排**（M2-5 表 B 的 B1/B2/B4/B5/B6-B11 与本轮无关，
   一字未动）；也不覆盖 A8 的 bounds 族调用点。
6. **不覆盖 GPU / Vulkan / 实机画面 / 性能**：未部署、未启动游戏、未做玩家前台验收；
   本轮全部证据为宿主机差分 + 静态门禁 + 构建，**不构成稳定版依据**，也**不得**表述为
   "palette 侧职责已迁完"（M2-5 的 B 组余项仍未做）。
7. **不 git 写**：所有改动停留在未提交工作树；本文记录的是**工作树文件哈希**。

## 10. 与其它文档的边界

- 本文不改 M1/M2-1/M2-2/M2-3/M2-4/M2-5/M2-5B 的等价记录与六份既有 .inc
  （只按"迁移必须同步"的规则改了 M2-4 门禁里的 A9/A10 死代码断言，见 §7）。
- 本文不是"M2-5 已完成""palette 侧职责已迁完"或稳定版依据；它只落地了 **A9/A10**
  两条死代码裁定。

## 11. 文档写入后的复跑

本记录、进度日志与 DEVELOPMENT_CHANGELOG.md 写入完成后，再在**同一棵树**重跑
ninja -n、预算门禁、六条等价门禁（M1/M2/M2-3/M2-5/M2-4/M2-5B）与 A9 门禁，要求
全部 exit 0。

- 复跑窗口（机器本地时钟）：见 2026-09-18-a9-a10-full-gate-rerun.log 的
  `##### DOC-WRITTEN RE-RUN` 段。
- 结果：ninja -n `no work to do.`（EXIT 0）→ 预算门禁 EXIT 0（冻结 **157**；行首站点
  165→112→112；已迁出 46；device.cpp **567**）→ M1 EXIT 0 → M2 EXIT 0 → M2-3 EXIT 0 →
  M2-5 EXIT 0 → M2-4 EXIT 0 → M2-5B EXIT 0 → **A9 EXIT 0** → 全量静态
  `STATIC_TOTAL=254 STATIC_FAIL=0`（EXIT 0）→ 复跑 ninja -n `no work to do.`（EXIT 0）。
- 复跑窗口内 DLL 身份不变：**36,278,629 B / 824AA63B…**（04:59:02；写入文档不触发重编译）。
- 诚实标注（顺序）：§8 的表格与本记录主体在本次复跑**之前**写入；复跑只确认"写文档
  没有破坏任何门禁"，未再改动任何源码或门禁。
