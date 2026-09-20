# M2-5 余项记录：表 B 余项裁定 + B4 聚合块逐字节迁出

> 状态：**完成（§1-§12 全部闭合）**。本轮先做**可行性逐条裁定**（§1），只对被判定为
> 可做的那一条（B4）执行迁移；legacy 参考、宿主差分（7 字段逐调用整数精确相等 +
> 每调用整结构 memcmp + 12 场景独立期望值表）、预算门禁 FROZEN_M2_5B、独立 M2-5B
> 等价门禁与 **2 条变异真跑**全部闭合；全量门禁在**最终树**复跑留档。
> 本轮**不部署、不启动游戏、不 git 写、不触碰 E:\Work 下任何文件、不称稳定版**。
> 原始门禁输出：docs/plan/2026-09-18-m2-5-remainder-full-gate-rerun.log。

工单：docs/plan/2026-09-18-overnight-execution-plan.md §2、docs/plan/2026-09-18-deepseek-overnight-handoff.md §4。
设计清册：docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md §1.2 表 B（B1/B2/B4/B5/B6-B11）、
§1.1 表 A 的 A9/A10、§1.4 执行顺序、§2（44 条计数器与三层门禁设计）、§3.2、§4-2、§4-3。
同构先例：…-m2-1-…、…-m2-2-…、…-m2-3-…、…-m2-5-taxonomy-extraction-record.md、
…-m2-4-migration-equivalence-record.md（最新范式）。

## 0. 结论摘要

- **表 B 逐条裁定（先判断，后动手）**：B4 **可做**并已迁出；**B1/B2 不可做**（强依赖 device
  状态 + 无法注入的时序）；**B5 不可做**（所需类型嵌套在 D3D9DeviceEx 内）；**B6-B10 不是可独立
  迁移项**（都是已迁出定义在 device.cpp 侧的调用点，其归属由其外层编排决定）；**B11 不可做 /
  归类待裁定**（compose-policy 与 device 资源缓存混合）。逐条理由与行号锚见 §1.2。
  **本轮没有硬做任何一条，也没有降低证据标准。**
- 唯一迁出：device.cpp pre-M2-5B :22200-:22244 的 **45 行**「每帧 submitted skinned palette
  聚合」块（Phase 7.48，清册表 B 的 B4），逐字节迁入**新模块**
  src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp} 的纯函数
  dxvk::war3::semantic::War3AggregateSubmittedSkinnedPalette(War3ShadowCaptureStats& st, bool skinned)。
- **唯一机械差异**：块内的 st 引用成为函数第一个形参（名字逐字相同），正文 45 行
  **零行删除、零 token 改写**；模块 .cpp 的 BEGIN/END 标记之间正文与 legacy 参考正文
  **2,421 B 双向 fail-closed 逐字节相同**。
- 所有输入**显式传参**；模块内**不出现**任何 device 成员、registry / hook 全局
  （迁移脚本 fail-closed 扫描 m_war3Scene / m_war3Semantic / m_state /
  VisibleRenderableRegistry::instance / this-> ，0 命中）。
- 宿主差分：**N/N 全过**（default 39,268,451/39,268,451；M2-5B 电池独占 1,347,808 条），
  7 个字段逐字段整数精确相等 + 每调用 memcmp + 12 场景 × 2 组 env 的独立期望值表。
- 两条变异真跑：语义条件反转 ⇒ 差分红（318,516 条 DIFF，逐字段点名）；可编译回流 ⇒
  预算门禁点名 War3AggregateSubmittedSkinnedPalette: device.cpp=1 > budget=0。
  两条都完成「改→构建→跑→红→还原→touch→真实重编译→复跑绿」，源码 SHA-256 逐字节还原。
- 全量门禁在最终树全绿（ninja -n no work、构建 exit 0、预算/M1/M2/M2-3/M2-5/M2-4/M2-5B
  门禁 exit 0、meson 83/83、全量静态 253/253、记录器 23/23、成本 PASS(38/38)、
  生命周期 187/187、解析器静态 94/94、根读方 55/55、往返 CERTIFIED）。
- **A9 War3SemanticBuildWorldPaletteIfNeeded 与 A10 War3SemanticVectorStorageReadable
  两条死代码本轮未迁、未删**；裁定建议与证据见 §9。

## 1. 可行性判断（逐条可做 / 不可做 + 理由）

### 1.1 判据口径

本轮沿用清册与工单的口径：一条表 B 条目算「可做」，必须同时满足

1. 被迁文本能**逐字节**搬进 src/d3d9/war3/semantic/ 下的模块（否则逐字节等价证据不成立）；
2. 宿主（war3_live_palette_selection_test 这一条只链接模块 .cpp 的 TU）能用**同输入域**
   对照 legacy 整函数与我方整函数，比较全部可观测输出；
3. 被迁单元不依赖**只存在于 d3d9_device.cpp / D3D9DeviceEx 内部**的符号、类型或时序。

任一条不满足即判**不可做**，只报告理由，不改代码。

### 1.2 逐条裁定

| # | 条目（post-M2-4 行号） | 裁定 | 关键依据 |
| --- | --- | --- | --- |
| B1 | live palette refresh 编排块 :20725-:20791 | **不可做** | 见下 (a)(b)(c) |
| B2 | 提交来源状态装配 :20718-:20724 + :20792-:20799 | **不可做** | 与 B1 同一上下文；本身不是单元 |
| B4 | 每帧 skinned palette 聚合 :22200-:22244 | **可做（已迁）** | 纯函数：只读写作参数 st 与 skinned |
| B5 | direct-caster palette churn :22245-:22343 | **不可做** | 类型嵌套在 D3D9DeviceEx 内 |
| B6 | War3TryBuildLiveRuntimeGroupPalette 调用点 :6629/:20360/:20755/:26171/:26251 | **非独立迁移项** | 定义已迁（M2-2）；调用点属外层编排 |
| B7 | War3NoteLivePaletteMotion 调用点 :20782/:26226/:26281 | **非独立迁移项** | 同上（M2-3） |
| B8 | War3NoteDrawTimePoseMotion 调用点 :19094 | **非独立迁移项** | 落在 M4/C3 领地（draw-time producer） |
| B9 | War3NoteSubmittedPaletteMotion 调用点 :21078 | **非独立迁移项** | 落在 append 主函数编排内 |
| B10 | War3SemanticHashMatrixPalette 调用点 :6881/:18659/:20232/:20383/:21075 | **非独立迁移项** | 定义已迁（M2-1） |
| B11 | War3GetOrCreateSemanticShadowPalette 定义 :18556… / 调用点 :21106 | **不可做（归类待裁定）** | 混合 device 帧缓存 + 资源原语 |

**(a) B1/B2 不可做的第一条硬理由：块内嵌 device.cpp 私有的 inline 计时助手。**
块内（:20751-:20752）构造

~~~
      auto livePaletteScope = War3SemanticSubmitScope(
          "War3SemanticScene/SubmitFrame/LivePaletteBuild");
~~~

而 War3SemanticSubmitScope 是 **d3d9_device.cpp:2732-2739 的 inline 函数，全仓没有任何头文件
声明或定义它在头文件里**（在 src 下以 include=*.h 搜 War3SemanticSubmitScope：**0 命中**），
它又读进程单例 war3::War3PerfMonitor::instance()（:2738）与 device.cpp 私有的
War3SemanticSubmitBreakdownEnabledFast()（:1253）。要把它整块迁进模块，只有两条路：
(i) 把这个助手（及其 gate）也搬到共享头 —— 那是**块外**的 device.cpp 结构性改动，
并会改变其余 ~40 个调用点的解析方式；(ii) 改写块内文本 —— 破坏逐字节等价。
两者都与「只动编排、逐字节等价」的口径冲突；而且该助手的可观测效果是 **perf scope 的
墙钟计时**，宿主差分无法对照（**无法注入的时序**）。

**(b) B1/B2 不可做的第二条硬理由：不是自足单元，输出被同一 2,000+ 行 device 函数消费。**
块的输入/输出是 :20183-:20187 声明的 5 个局部量（liveRuntimeGroupPalette /
liveMaxVertexGroupSlot / liveRuntimeGroupPaletteHash / liveRuntimeRawPaletteHash /
liveRuntimePoseModelPtr）与 :20718-:20724 的 4 个局部量，写 m_war3Scene.shadowStats
与 selectedPalette，其结果随后被 :20821-:20829 的 canonicalInputs 与 :22025-:22026 的
per-frame dynamic hash 使用。要让「legacy 整函数 vs 我方整函数」成立，宿主就得复刻整个
War3TryAppendSemanticShadowPacket 上下文（device 成员、hook、registry、NativeD3D9BackendRuntime），
即工单所点的**强依赖 device 状态**情形。

**(c) B1 的核心动作依赖全局单例。** 块的核心调用是 War3TryBuildLiveRuntimeGroupPalette，
其定义（M2-2 迁出，模块 .cpp :173 起）内部 6 处引用 dxvk::war3::model::PoseRegistry
（:331/:540/:578/:580/:773/:791），即依赖模型 hook 在运行时发布的全局姿态状态。宿主电池
两侧只能共享同一个替身单例，无法给两侧独立输入域（M2-2 正是靠替身驱动，但那是「链本体」的
覆盖，不是「外层编排 + 同一全局」的覆盖）。B2 是 B1 的输出声明与两个三元式，无独立内容。

**B4 为什么可做**：:22200-:22244 的 45 行只做一件事——读 st.semanticSceneDirectLastSubmittedPaletteHash
（st = 调用点 :22172 的 auto& st = m_war3Scene.shadowStats; 别名）与形参 skinned，滚动写回 7 个
stats 字段，另调用纯模板 bit::fnv1a_iter（util/util_bit.h:728）。没有 device 成员、没有 registry /
hook、没有 thread_local、没有计时、没有顺序以外的时间依赖。因此它满足判据 1/2/3：
正文零改写搬入模块即可，宿主两侧各持独立 War3ShadowCaptureStats 就能穷举输入域对照。

**B5 为什么不可做**：churn 块读写成员 m_war3SemanticDirectCasterContracts（d3d9_device.h:2976），
其类型 War3SemanticDirectCasterContractMap 与配套的 Key / KeyHash / State
（d3d9_device.h:2460/:2477/:2486/:2496）**嵌套在 class D3D9DeviceEx 内**（该类 :387 起、:3194 闭合）。
模块要拿到这个类型，只能（i）include d3d9_device.h（语义模块刻意不引重头，且会拖入 D3D 设备面），
或（ii）把这些类型搬到共享头（= 改 device.h 公开面）。两者都不是「逐字节搬编排」，
因此本轮判定不可做（若将来主线程愿意先做类型外移，这条可另立一次改动）。

**B6-B10 为什么不是独立迁移项**：这些符号的**定义**早在 M2-1/M2-2/M2-3 就已在模块里；
device.cpp 里剩下的是调用点。调用点文本一个字节都没改，仍按原样经 using-directive 解析到
模块定义（M1 机制）。「把一个调用点搬走」只有在连带搬走其外层编排时才有意义：B6/B7 在
B1 块与 leased-packet 路径内、B8 在 M4/C3 的 War3TryPublishSemanticDrawTimePose 内、
B9 在 append 主函数内、B10 在 packet 构建与 canonical 路径内。**这不是漏做，而是判定为
「无独立可迁内容」并留待各自外层编排片处理。**

**B11 为什么不可做**：定义（:18556 起）内部同时含 compose-policy 判定（A4/A5，已迁模块）、
device 帧内缓存 m_war3SemanticPaletteCache 与资源原语 War3GetOrCreateShadowMatrixPaletteFromData
（清册 C2）。清册 §4-1 已把它标为「归类待裁定」，本轮核实后将裁定建议留 §9.3，
**不夹带迁移**。

## 2. 唯一迁出项 B4：范围、落点与字节账

### 2.1 逐字节范围

| 项 | 值 |
| --- | --- |
| 块范围（pre-M2-5B device.cpp） | :22200（if (skinned) {）- :22244（其配对 }） |
| 块规模 | **45 行 / LF 2,420 B / CRLF 2,464 B**（含中文注释） |
| 花括号配对复核 | 迁移脚本与生成器**各自**从 LOCATE 注释起做深度配对，两侧都唯一落在 :22200-:22244 |
| 迁出后 device.cpp | 2,250,786 B → **2,249,192 B**（-1,594 B） |
| 字节账闭合 | -2,464（块）+ 531（锚注释）+ 63（include 行）+ 276（using-directive 说明行）= **-1,594**，与实测逐字节相等 |
| 反向重建 | migrate --reconstruct-check：从 post device.cpp 重建 pre → **reconstruct == pre snapshot: True（2,250,786 B vs 2,250,786 B）** |

### 2.2 落点与命名理由

落点 = **新模块** src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp}：

1. 与已迁模块同目录、同命名空间 dxvk::war3::semantic、同 using-directive 解析机制；
2. 它是对 stats 的**聚合/发射记账**责任（清册 §2.1 分组 4 的 7 条计数器），与
   war3_palette_taxonomy_emission（B3）同族但**不修改既有 B3 模块**——新模块避免重钉
   M2-5 门禁里已登记的模块 .cpp/.h SHA（那会把一次加法式迁移变成对既有证据的改写）；
3. 一处迁移 = 一处 provenance：.inc 与模块正文的一致性只需比对同一段文本。

函数名 War3AggregateSubmittedSkinnedPalette **不匹配**预算门禁的 SEMANTIC_FAMILY_RE
（无 Resolve/Select/Runtime/Key/… 后缀）⇒ 判定 3 对它盲，必须显式登记 FROZEN_M2_5B（§6）。

### 2.3 边界（不动的部分）

诊断门与 casterKey 前置条件留在 device.cpp：提取的是 :22170 的
if (War3SemanticPaletteDiagnosticsRuntime() && stableAuthoritativeSkinnedGeometryKey &&
currentDrawSample != nullptr) 门内、:22185-:22197 的 caster key 记账之后的那个
if (skinned) { … } 子块。调用点文本为：

~~~
      // M2-5B（2026-09-18）：per-frame submitted skinned palette 聚合（清册表 B 的
      // B4，7 个 stats 字段 + 顺序敏感滚动 FNV-1a）已逐字节迁往
      // src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp} 的
      // War3AggregateSubmittedSkinnedPalette。诊断门与 casterKey 前置条件等调用点
      // 编排留在 device.cpp 原位；等价证据见
      // docs/plan/2026-09-18-m2-5-remainder-record.md。
      War3AggregateSubmittedSkinnedPalette(st, skinned);
~~~

## 3. provenance（fail-closed）

| 项 | 值 |
| --- | --- |
| pre-M2-5B 工作树 src/d3d9/d3d9_device.cpp | **2,250,786 B / D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811** |
| 捕获时间（机器本地时钟） | 2026-09-18T04:15:05 |
| %TEMP% 备份 | %TEMP%\device_pre_m2_5b.cpp（同 SHA，脚本比对 match True） |
| 交叉验证 | 该 SHA **逐字节等于** M2-4 记录 §9.1 的「迁移后 device.cpp」⇒ M2-5B 的迁移前状态就是 post-M2-4 工作树，闭合 |
| worktree-vs-git-blob | 与 M1-M2-4 相同：工作树文件哈希，**不是** git blob（均未提交） |

同一次捕获的其它备份（%TEMP%）：meson_pre_m2_5b_d3d9.build、
test_pre_m2_5b_war3_live_palette_selection_test.cpp、gen_pre_m2_5b_legacy_reference.py、
budget_pre_m2_5b_static.py；迁移后另有 device_post_m2_5b.cpp、
module_post_m2_5b_submitted_aggregation.cpp、test_post_m2_5b_war3_live_palette_selection_test.cpp（变异还原用）。
一次性迁移脚本（不入库，审计用）：D:\tmp\m2_5b_work\m2_5b_migrate.py（可 --apply / --reconstruct-check /
--measure-baseline 重放）。

**baseline 实测（FROZEN_M2_5B）**：用预算门禁**同一个** DEF_RE 与同一条「第 0 列起始、
非注释行」规则（门禁内从预算门禁源文件 eval 出同一个 regex，不抄写）扫 pre-M2-5B 快照：
War3AggregateSubmittedSkinnedPalette 的行首站点 **0**、全文出现 **0** 次；该块 45 行全部
处于缩进层，不含任何第 0 列起始站点。⇒ baseline = 0、budget = 0，与迁移**同一次改动**落地。

## 4. legacy 参考与双向 fail-closed 生成器（--m2-5b）

- 新文件 src/d3d9/war3/semantic/tests/war3_palette_submitted_aggregation_legacy_reference.inc
  = **4,952 B**，SHA-256 C6D32B4EF8DCCC27A50EE407037687F190CD3D72FC90FE053EB4A49D7E3EE46F。
- 由入库生成器 AutoTest/gen_war3_live_palette_selection_legacy_reference.py **新增 --m2-5b 模式**产出；
  **M1/M2-1/M2-2/M2-3/M2-5/M2-4 六份 .inc 与其 SHA 一字未动**（本轮用 --m2-4/--m2-5 重跑，
  分别复现 E9CB8931…/9397 B 与 A5333AC7…/22511 B，逐字节相同）。
- 生成器 fail-closed 三步：(1) 快照 SHA-256/字节数必须等于登记常量（D0E80399…/2,250,786）；
  (2) 块必须花括号配对到 :22200-:22244、起点必须是 skinned 守卫、正文不得含 device/registry token；
  (3) **模块 .cpp 的 BEGIN/END 标记之间正文必须与快照抽取结果逐字节相同**（实测 2,421 B）。
  生产运行输出：generated src\d3d9\war3\semantic\tests\war3_palette_submitted_aggregation_legacy_reference.inc /
  size: 4952 / SHA-256: C6D32B4EF8DCCC27A50EE407037687F190CD3D72FC90FE053EB4A49D7E3EE46F。
- 命名空间改写规则（除 LF 归一外唯一的机械编辑）：外层改为 m2_legacy_reference；正文
  **零 token 改写**——块只读写第一个形参（名字 st，与 device.cpp 逐字相同）并调用无限定的
  纯模板 bit::fnv1a_iter，后者经 .inc 头部 using namespace dxvk; 解析到 dxvk::bit，与
  pre-M2-5B device.cpp 的真实调用目标一致。dxvk::War3ShadowCaptureStats 是**共享真实契约类型**
  （两侧同一份头文件，刻意不重写）。

## 5. 宿主差分等价测试

差分测试：src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp
（M2-1/M2-2/M2-3/M2-5/M2-4 既有断言一字未动的**加法式扩展**；meson 目标名不变，
只把新模块 .cpp 加进同一 test 目标与 dll 源列表 ⇒ meson 目标数不变）。

### 5.1 结构与比较口径

- 两侧各持**独立** War3ShadowCaptureStats；static_assert 平凡可复制。
- **每个用例**除 7 个字段逐字段整数精确相等外，还对整个 War3ShadowCaptureStats 做一次
  memcmp（捕获「写到了这 7 个字段之外」）。每用例 = 1 memcmp + 7 字段 = **8 条断言**，容差 = **0**。

### 5.2 场景电池

| 电池 | 规模 | 覆盖 |
| --- | --- | --- |
| 系统网格 | 18,432 用例（147,456 断言） | lastSubmittedHash 8 种（0 / 1 / 全 1 / FNV offset / 最高位 / 低 32 全 1 / 高 32 全 1 / 0x0123…）× first 2 × combined 3 × runLast 3 × run 4 × distinct 2 × max 4 × zero 2 × skinned 2 |
| 定长序列 | 44 步（352 断言） | 同 key 连续帧、交替、零 key 注入、run 增长后切换、全 1 hash、FNV offset hash |
| 定种子随机世界 | 150,000 步（1,200,000 断言） | splitmix64 随机 hash（含 0 注入与 8 值池）、随机 skinned，**跨步保持滚动状态**（状态机漂移只有长序列能咬） |
| 合计 | **1,347,808 断言** | 测试自身打印 M2-5B battery: 1347808 checks, 0 failures |

### 5.3 实测（最终树，四矩阵）

| env 矩阵 | 结果 |
| --- | --- |
| default | 39,268,451/39,268,451 checks passed，EXIT=0；M2-5B battery: 1347808 checks, 0 failures |
| DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1 | 39,268,450/39,268,450，EXIT=0 |
| DXVK_WAR3_SKIN_PALETTE_CONTRACT=1 | 35,780,339/35,780,339，EXIT=0 |
| …CONTRACT=1 + …DIAGNOSTICS=1 | 35,780,338/35,780,338，EXIT=0 |

（检查数是**全文件累计**，含 M2-1/M2-2/M2-3/M2-5/M2-4 既有电池。对照 M2-4 记录：
default 37,920,643 → 39,268,451 的差 1,347,808 恰为本轮 M2-5B 电池量。）

### 5.4 --probe-m2-5b 独立期望值表

12 个固定场景 × 2 组 env（default / diagnostics-on），逐场景要求 module == legacy **全部 7 字段**
相等，并与门禁内**独立实现**的 FNV-1a 推导逐字段核对（不是与 legacy 互相印证）。
门禁的独立推导按源码语义复算：(curPaletteHash == 0 → zero++；first 空则取当前；
combined==0 → 取 hash 或 0x9E3779B97F4A7C15、distinct=1、runLast=hash、run=1、max=1；
否则 combined = fnv1a_iter(fnv1a_iter(combined, lo), hi)，runLast 相同则 run++ 并按需抬高 max、
不同则 distinct++ 且 run=1)；FNV-1a 步进在门禁内以 (h ^ v) * 0x100000001B3 mod 2^64 独立实现。
12 行实跑与独立推导**全部一致**（原始 12 行见 full-gate log [18]；module 与 legacy 两侧也逐字段相同）。

## 6. 独立 M2-5B 等价门禁

新文件 AutoTest/test_war3_palette_m2_5b_equivalence_static.py（全量静态 252 → **253**）。
静态 fail-closed：

1. legacy 参考的 provenance 文本（pre SHA/字节数/捕获时间/符号/7 字段）与自身 SHA/字节数；
2. 模块 .cpp/.h SHA-256 与登记值；
3. **本门禁自己**从模块 .cpp 与 .inc 各抽 BEGIN/END 之间正文逐字节比较；
4. 若 %TEMP%\device_pre_m2_5b.cpp 在位：独立从快照重抽块（花括号配对 + 行号）与模块正文比对，
   并用预算门禁**同一 DEF_RE**（从其源码 eval）实测 baseline=0；不在位则打印说明（不静默）；
5. device.cpp：符号调用点恰好 1 处、符号出现 2 次（锚注释 + 调用）、include 行在位、
   7 个字段名 0 次、块首文本已不在 device.cpp；
6. d3d9_war3_scene.h：7 个字段名各恰好 1 次；模块 .cpp：7 个字段名出现次数逐名实测并钉死
   （1/2/4/2/3/5/3，多累加点与比较点不得被合并或复制）；
7. FROZEN_M2_5B 键集合与 (0, 0)；meson dll 源与 test 目标都链接新模块 .cpp；
8. 生成器 --m2-5b 模式原语在位；差分测试 include / sem:: / legacy:: 调用点 / 必需原语在位；
9. 动态：4 组 env 的 N/N + M2-5B battery **精确值 1,347,808** 与 0 failures；
   --probe-m2-5b 12 场景 × 2 组 env 与独立期望值表逐行相等。

门禁自报（final tree）：
war3 palette m2-5b equivalence gate static checks passed（M2-5B 已迁出符号 1 个；7 个字段单一实现；
legacy 参考 4952 B / SHA-256 C6D32B4EF8DCCC27…；模块正文与 legacy 正文逐字节相同 2421 B；
pre-M2-5B device.cpp 2250786 B / D0E80399703CBF3B…；快照独立重抽一致 + DEF_RE baseline=0；
probe-m2-5b 场景 12/12 × 2 组 env；M2-5B 电池 1347808 断言（下限 1200000）；
差分总检查 default=39268451 diagnostics-on=39268450（下限 30000000））。

## 7. 变异验证（2 条真跑）

### 7.1 变异 A：模块侧语义条件反转（差分必须红）

- 改：模块 .cpp 的 if (curPaletteHash == 0u) { → if (curPaletteHash != 0u) {（ZeroHash 计数条件反转）。
  变异后模块 SHA 39911F33DBB2872D39D4170CF0F9C528FD745E79D33BC5C70DBA0E731794D372（3,479 B）。
- 构建：**NINJA_EXIT=0**，日志尾 [3/4] Linking target src/d3d9/war3_live_palette_selection_test.exe /
  [4/4] Linking target src/d3d9/d3d9.dll ⇒ **能编译**（正是「语义漂移但不报错」的回归类型）。
- 跑（红）：EXIT=1，M2-5B battery **1347808 checks, 318516 failures**，
  全文件 38,949,935/39,268,451；stderr **318,516 条 DIFF**，首 6 行：
  DIFF line 4054 M25BGrid: M2-5B stats memcmp mismatch /
  DIFF line 4054 M25B.ZeroHash: module=0 legacy=1 /
  DIFF line 4054 M25BGrid: M2-5B stats memcmp mismatch /
  DIFF line 4054 M25B.ZeroHash: module=5 legacy=6 /
  DIFF line 4054 M25BGrid: M2-5B stats memcmp mismatch /
  DIFF line 4054 M25B.ZeroHash: module=0 legacy=1。
- 还原：%TEMP%\module_post_m2_5b_submitted_aggregation.cpp 复制回 + LastWriteTime 置当前时刻；
  SHA 83A241506B111B99CBA49E30E8228D48796E2004BFA745362EDCD3F1A993858D **逐字节相同（MATCH True）**；
  真实重编译（[1/4]-[4/4] 均出现新编译/链接行）后复跑 39,268,451/39,268,451、EXIT=0。
- **诚实标注**：本轮未把「预算门禁在变异 A 下仍绿」作为证据（M2-4 已实测该事实）；
  本轮只登记差分红的原始输出。

### 7.2 变异 B：职责回流（预算门禁必须点名）

- 改：在 device.cpp 的 using namespace dxvk::war3::semantic; 之后插入
  namespace warvk_m2_5b_reflow_probe { <模块正文逐字节> }（定义行从第 0 列开始；
  具名命名空间避免与 using-directive 导入的模块声明歧义）。变异后 device.cpp = **2,251,841 B /
  37F8BDA3B6101B477B2F627D827FEB69672BA1E9E04A6161565509A5E73D1D34**（+2,649 B）。
- 构建：**NINJA_EXIT=0**，日志尾 [2/2] Linking target src/d3d9/d3d9.dll ⇒ **可编译回流**。
- 预算门禁（红并**点名**）：exit 1，
  AssertionError: d3d9_device.cpp 中语义选择职责符号超过冻结预算（职责回流）：
  War3AggregateSubmittedSkinnedPalette: device.cpp=1 > budget=0。
- 独立 M2-5B 门禁**也**红（exit 1）：device.cpp 的 War3AggregateSubmittedSkinnedPalette 调用点不是恰好 1 处
  ⇒ 本轮这条回流有**两个**捕手；即使 M2-5B 门禁不存在，FROZEN_M2_5B 的判定 1 仍能单独点名
  （这正是必须登记的原因：该函数名不匹配语义命名族，判定 3 盲）。
- 还原：SHA AC5FB37E3BFE65BA8DA47E173C354EF8E405E4EEA95A1CB417212020EA80E380
  **逐字节相同（MATCH True）**；真实重编译（[1/2] Compiling … d3d9_device.cpp.obj / [2/2] Linking … d3d9.dll）
  后预算门禁 exit 0（冻结符号 156；行首站点 迁移前 165 -> 迁出后上限 112 -> 当前 112；已迁出 46；
  device.cpp 站点 568）。

### 7.3 还原完整性（SHA-256 证明）

| 被变异文件 | 变异后 SHA-256 | 还原后 SHA-256 | 逐字节相同 |
| --- | --- | --- | --- |
| 模块 war3_palette_submitted_aggregation.cpp | 39911F33…94D372 | 83A241506B111B99CBA49E30E8228D48796E2004BFA745362EDCD3F1A993858D | **是** |
| d3d9_device.cpp | 37F8BDA3…D1D34 | AC5FB37E3BFE65BA8DA47E173C354EF8E405E4EEA95A1CB417212020EA80E380 | **是** |

**构建陷阱（M2-3/M2-5/M2-4 已实测，本轮复核并规避）**：Copy-Item 保留源文件 mtime 会让 ninja 判
no work，只比源码 SHA 会得到假绿/假红。本轮还原一律显式置 LastWriteTime，并以构建日志里的
真实编译/链接行确认二进制来自还原后的源码。

## 8. 全量门禁复跑（最终树）

窗口：两条变异均已还原、源码 SHA 与 §3/§7.3 一致。原始输出
docs/plan/2026-09-18-m2-5-remainder-full-gate-rerun.log。

| # | 门禁 | 原始结果 |
| --- | --- | --- |
| 0 | 源码身份 | 见 §8.1 |
| 1 | ninja -C build32 -n | ninja: no work to do.，EXIT=0 |
| 2 | 最终树真实重建（Below Normal + -j2） | NINJA_EXIT=0（[5/5] Linking target src/d3d9/war3_live_palette_selection_test.exe） |
| 3 | 预算门禁 | EXIT 0：冻结符号 156；行首站点 迁移前 165 -> 迁出后上限 112 -> 当前 112；已迁出 46 个符号；device.cpp 行首定义站点总数 568 |
| 4 | M1 等价门禁（一个字节不改） | EXIT 0：已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / 2EA43F97… |
| 5 | M2 等价门禁 | EXIT 0（M2-1 8 + M2-2 链 1；probe 6 + probe-chain 3 组 × 10；motion-on 下限 12000000） |
| 6 | M2-3 等价门禁 | EXIT 0（motion 3 + 2 Entry；参考 8406 B / 7DF61688…；motion-on 差分 39268450（下限 12000000）） |
| 7 | M2-5 等价门禁 | EXIT 0（taxonomy 1 符号 / 34 字段；参考 22511 B / A5333AC7…；正文逐字节 18937 B；diagnostics-on 差分 39268450（下限 21000000）） |
| 8 | M2-4 等价门禁 | EXIT 0（8 符号 / 9 定义；参考 9397 B / E9CB8931…；probe-m2-4 20 × 2；电池精确值 7477257） |
| 8b | **M2-5B 等价门禁（本轮新增）** | EXIT 0（见 §6 自报） |
| 9 | meson test -C build32 --num-processes 2 | Ok: 83  Fail: 0，EXIT=0（83→83：加法式扩展，未新增 meson 目标） |
| 10 | 全量静态 AutoTest/test_*_static.py 逐个 | STATIC_TOTAL=253 STATIC_PASS=253 STATIC_FAIL=0（252→253：新增 M2-5B 证据门禁） |
| 11 | 记录器 war3_palette_object_evidence_test | SUMMARY: 23 passed, 0 failed，EXIT=0 |
| 12 | 成本 war3_palette_object_evidence_cost_test | COST_VERDICT=PASS checks=38 failures=0，EXIT=0 |
| 13 | 生命周期 war3_shadow_build_lifecycle_test | SUMMARY: … 187/187 case(s) passed，EXIT=0 |
| 14 | 解析器静态 AutoTest/test_palette_object_evidence_analysis_static.py | Ran 94 tests … OK，EXIT=0 |
| 15 | 根读方 AutoTest/test_analyze_frame_evidence.py | Ran 55 tests … OK，EXIT=0 |
| 16 | 往返 py AutoTest/test_palette_object_wire_roundtrip.py | CHECKS=1107 FAILURES=0；ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED（A-F）；EXIT=0 |
| 17 | 差分四矩阵 | §5.3（四组全部 EXIT=0，M2-5B 电池四组均 1347808 / 0 failures） |
| 18 | --probe-m2-5b 两组 env | 12 行 × 2 组，module == legacy 且与门禁独立推导一致 |
| 19 | DLL 身份 | §8.2 |
| 20 | 复跑 ninja -C build32 -n | ninja: no work to do.，EXIT=0 |

**一个需要点名的既有事实（与 M2-4 §9 同类）**：M2-3 门禁打印的 motion-on 差分数与 M2-5 门禁打印的
diagnostics-on 差分数都是**全文件累计**检查数，本轮由 37,920,642 变为 39,268,450；两条门禁的登记
下限（12,000,000 / 21,000,000）仍满足，门禁本身未改。M2/M2-3/M2-5 三份记录的措辞在解读该数字时
需按本条更新。

### 8.1 源码与产物身份（最终树）

| 项 | 值 |
| --- | --- |
| src/d3d9/d3d9_device.cpp | 2,249,192 B / AC5FB37E3BFE65BA8DA47E173C354EF8E405E4EEA95A1CB417212020EA80E380 |
| src/d3d9/war3/semantic/war3_palette_submitted_aggregation.h | 2,124 B / 3907893315D33A0D9DB0CB2F201D63BA76D804A92E6D27A005F793557B4E1E6A |
| src/d3d9/war3/semantic/war3_palette_submitted_aggregation.cpp | 3,479 B / 83A241506B111B99CBA49E30E8228D48796E2004BFA745362EDCD3F1A993858D |
| …/tests/war3_palette_submitted_aggregation_legacy_reference.inc | 4,952 B / C6D32B4EF8DCCC27A50EE407037687F190CD3D72FC90FE053EB4A49D7E3EE46F |
| …/tests/war3_live_palette_selection_test.cpp | 168,483 B / 2B8C2ECBC466F39727FA2D367376B5C10556058E0A80DD7CD945B8D4A11ECE03 |
| src/d3d9/meson.build | 56,994 B / B225BD44E07026A8D0BC236E5AACEEF91FA0518FB3B4D0BF11E9FBE77EF806BF |
| AutoTest/gen_war3_live_palette_selection_legacy_reference.py（加 --m2-5b） | 55,863 B / B5CC80C0887A63E5A96C6426BC8DB0E8887460C19CAC173332E2B50E4CDE6142 |
| AutoTest/test_device_semantic_responsibility_budget_static.py（加 FROZEN_M2_5B） | 24,341 B / 78C7C14AAC8F121B6F97D48A40DA523D0672449D5ED47DD608F1FD2214C6D631 |
| AutoTest/test_war3_palette_m2_5b_equivalence_static.py（新增） | 21,142 B / FA71B49D6E9D8DA647F729D5BC25E28EB14F0D6F3C792B376A08759B150AFE09 |
| build32/src/d3d9/war3_live_palette_selection_test.exe | 6,540,197 B / 3807D5924EF1C25AE85837B6E9DB6F6EF7BD8FAFB309A7447A3B342BBC745BE0 |

### 8.2 DLL 身份与可复现性

最终产物 build32/src/d3d9/d3d9.dll = **36,278,340 B**，
SHA-256 **7DA555A781E54C4BFEF320105FA0AF5F301DC54A8FD13F2D97CB08FA92E87216**
（最终树最后一次真实构建：touch 新模块 .cpp 与测试 .cpp 后日志出现真实编译与
[5/5] Linking target src/d3d9/war3_live_palette_selection_test.exe）。对照 M2-4 的最终 DLL
（36,277,956 B / BDB47E4E…）⇒ 尺寸 **+384 B**：device.cpp 净减 1,594 B 源码、新模块 .cpp 3,479 B 源码
（含 BEGIN/END 标记与说明注释）、一次真实重链的链接时间戳。

**同尺寸不同 SHA 的实例（M2-3/M2-5/M2-4 已实测，本轮复核）**：同一最终源码在不同时刻链接
会得到同尺寸、不同 SHA 的 DLL（MinGW ld 写入链接时间戳）。因此「同源码 ⇒ 同尺寸」成立、
「同源码 ⇒ 同 SHA」**不成立**。本记录登记的是最终树**最后一次真实构建**的身份。

**未部署**：本轮没有覆盖任何游戏目录文件，未启动游戏或编辑器，E:\Work 下未触碰任何文件。

## 9. A9 / A10 死代码的裁定建议（本轮未迁、未删）

### 9.1 本轮实测

| 符号 | 定义 | 全 src 出现次数 | 调用 / 实例化 | 是否占 DEF_RE 行首站点 | 命名族 |
| --- | --- | --- | --- | --- | --- |
| A9 War3SemanticBuildWorldPaletteIfNeeded | device.cpp:7167（[[maybe_unused]] void） | **1**（仅定义） | **0 调用** | **0**（[[maybe_unused]] 前缀不以标识符开头） | 不匹配 |
| A10 War3SemanticVectorStorageReadable | device.cpp:7152（function template） | **1**（仅定义） | **0 实例化** | **0**（template 行不以标识符开头） | 不匹配 |

证据口径：全仓（排除 build*/归档产物）搜两个名字，命中只有 device.cpp 的定义、
war3_live_palette_selection.h:237 的一句说明注释、以及
AutoTest/test_war3_palette_m2_4_equivalence_static.py:182-185 的 DEAD_CODE_STILL_IN_DEVICE
断言文本（那是 M2-4 故意钉住「未被擅自删除」）。**没有**任何构建系统引用
（meson 源列表、.inc 包含、宏展开）指向这两个名字；AutoTest/artifacts 下的同名命中属于
归档的独立审阅包快照，不参与构建。

### 9.2 裁定建议（本轮不做，留待单独改动）

1. **A10 War3SemanticVectorStorageReadable：建议单独删除。** 它是已迁出的
   War3SemanticPaletteStorageReadable 的早期泛型版本，迁移后 0 实例化；删除不产生代码差异
   （模板不实例化）。删除必须是**单独一次改动 + 单独全量门禁**，并显式声明「本项是删除而非迁移」；
   同时必须同步修改 AutoTest/test_war3_palette_m2_4_equivalence_static.py 的
   DEAD_CODE_STILL_IN_DEVICE（第 184 行）——否则 M2-4 门禁会红。
2. **A9 War3SemanticBuildWorldPaletteIfNeeded：建议迁移（或显式登记保留），而不是删除。**
   它虽然 0 调用，但正文（:7167-:7181）描述了「model-local palette → world-space 组合」
   （outPalette.push_back(worldTransform * localMatrix)）这条**已被 A4 使用的合同**的完整写法，
   在 device.cpp 里是一份 v1.27a 语义参考；且它调用的 War3SemanticPaletteLooksModelLocal
   已在 M2-4 迁入模块，A9 正文可**逐字节**随迁到 war3_live_palette_selection.{h,cpp} 并配
   legacy 覆盖（命名族不匹配 ⇒ 必须登记 FROZEN_M2_4 增量项）。备选是「显式标注保留 +
   FROZEN baseline=1、budget=1」。
3. **本轮两件都没做**：清册 §1.4 第 5 条与工单都要求「先报告裁定建议」。上述两条各需
   单独一次改动 + 单独全量门禁（含 M2-4 门禁的 DEAD_CODE_STILL_IN_DEVICE 改锚），
   不得夹带进结构性迁移。

### 9.3 B11 的归类说明（沿用清册 §4-1，本轮核实后仍不建议迁）

War3GetOrCreateSemanticShadowPalette 的判定侧（A4/A5）已经随 M2-4 在模块里；定义体内剩下的是
device 帧内缓存（m_war3SemanticPaletteCache）与资源原语
War3GetOrCreateShadowMatrixPaletteFromData（清册 C2，资源生命周期）。把这段搬进语义模块会把
device 成员与资源池一起拖过去，属 M3 主题。**建议保持现状**，等 M3 或专门的
「device 帧缓存外移」改动再处理。

## 10. 未覆盖项（如实声明）

1. **B1/B2/B5/B6-B10/B11 本轮未迁、未改**（裁定见 §1）。这不是「已迁完」，
   也不得表述为「palette 侧职责已迁完」。
2. **调用点本身是新增文本**：B4 的调用点参数（st, skinned）是原局部量、语义等价，但「调用点
   文本」是新的；差分不覆盖「device.cpp 取到的实参是否与原块一致」——这只能由代码审阅 + 全量
   构建 + 后续实机门覆盖。
3. **7 个字段的出口链未由本门禁覆盖**（bridge / hub / control plane / perf ×2 写手 / 报告）；
   由既有静态门禁与全量静态承担。
4. **宿主替身**：本电池不需要 registry 替身（模块正文不访问任何全局）；但 bit::fnv1a_iter 是
   共享真实模板（同一份 util_bit.h），差分证明的是「同一模板下两个实现一致」。
5. **单线程**：本块无 thread_local、无跨帧表（这是它与 B3 的实质区别），因此不做多线程差分。
6. **env 覆盖为 4 个矩阵 + probe 2 组**而非穷举；本块本身不含 env getter，env 维度只是
   确认与既有门禁共存。
7. **变异 B 只证明门禁的点名能力**：那段回流代码没有调用者，因此不覆盖「回流后的运行行为」。
8. **--probe-m2-5b 的期望值表**是门禁内独立推导（含独立 FNV-1a）与实跑对照得到的回归 pin，
   不是第三方来源；它的价值是钉死绝对语义与发现「两侧同步漂移」，不是数学证明。
9. **本测试不覆盖 GPU / Vulkan / 实机画面 / 性能**；未部署、未启动游戏、未做玩家前台验收。
   本轮全部证据为宿主机差分 + 静态门禁，**不构成稳定版依据**。
10. **M2-4 的 A9/A10 断言（DEAD_CODE_STILL_IN_DEVICE）本轮未改**，两条死代码仍在原位。

## 11. 与其它文档的边界

- 本文**不改** docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md 的任何结论；它是设计与清册。
- 本文**不改** M1/M2-1/M2-2/M2-3/M2-5/M2-4 的等价记录、六份既有 .inc 与既有门禁的语义
  （本轮新模块刻意避开既有模块的 SHA pin，既有门禁一个字节未改）。
- 本文**不是**「M2-5 已完成」：M2-5 表 B 只完成了 B4（B3 由上一轮完成），B1/B2/B5/B6-B11 未做。
- 本轮**未** git 写、未部署、未启动游戏、未称稳定版、未触碰 E:\Work。

## 12. 文档写入后的复跑

本记录 §1-§11 与进度日志 / 开发台账写入完成后再跑一次完整门禁套件，要求全部 exit 0。结果：

- 复跑窗口（机器本地时钟）：见 docs/plan/2026-09-18-m2-5-remainder-full-gate-rerun.log 头部 date: 行。
- ninja -n no work（exit 0）→ touch 新模块 .cpp 与测试 .cpp 后真实重建 NINJA_EXIT=0 →
  预算门禁 exit 0（冻结 156 / 站点 165→112→112 / 已迁出 46 / 568）→ M1 exit 0 → M2 exit 0 →
  M2-3 exit 0 → M2-5 exit 0 → M2-4 exit 0 → **M2-5B exit 0** → meson Ok: 83 Fail: 0 →
  全量静态 253/253 → 记录器 23/23 → 成本 PASS(38/38) → 生命周期 187/187 →
  解析器 Ran 94 tests OK → 根读方 Ran 55 tests OK → 往返 CHECKS=1107 FAILURES=0 + CERTIFIED →
  差分四矩阵 EXIT=0 → probe-m2-5b 两组 env EXIT=0 → ninja -n no work。
- **诚实标注（顺序）**：§8.1 的测试 exe 身份与 §8.2 的 DLL 身份数字是在本文档写入后的复跑中测得并**回填**的；
  除该处与 §8 表格中由该次复跑产生的产品身份外，记录主体在复跑前已定稿。
  本轮未再改动任何源码或门禁（仅在复跑后回填本节、§8.1 的测试 exe 身份与 §8.2 的 DLL 身份）。
  （因此本记录文件的字节数/SHA 与 full-gate log [0] 行记录的版本会差这一次回填；本记录不自我引用自身哈希。）
