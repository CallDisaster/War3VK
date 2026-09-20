> ⚠️ **历史文档 —— 已过期（2026-09-18 round 79 标注）**
>
> 本文描述的是**当时**的现场与计划。**当前权威事实**见 `2026-09-18-objective-evidence-ledger.md`：
>
> ```
> 站点 E:\Work\Warcraft III\d3d9.dll = 36,288,789 B / F275545BAA65A015…5CF07FF3 = 基线 ⇒ **未部署**
> 写方 = **恒定 paletteObject version=4**（已删除 firstSightUsed() 动态选版本）
> 实机采集 = **未获授权、零采集**（协议见 2026-09-18-stageE-live-capture-protocol-v4.md）
> 门禁 = 静态 263/0（**不含**门禁外 40 个文件里的 3 个既有红）
> 五项裁定待决（窗口维度导出层 / 双链路由 / 实机授权 / 构建可复现 / 门禁外三红与覆盖缺口）
> ```
>
> **若本文与上述冲突，以上述为准。** 尤其是：本文若说"现场 DLL 已部署/勿动"，
> 那是**当时**的状态；**今天站点是基线、未部署**。
>
> ---
>
# 交接：目标①–④ 当前状态与后续动作（2026-09-18 汇总）

> 本文件是**唯一的状态入口**。带日期的 plan 文档是过程证据；本文件是"现在在哪、下一步做什么"。

## 0. 一句话状态

**① 闭环；② 已修且已复核；③ 主体完成并有实机 A/B 证据，剩若干收尾；④ 第一半 ✅，第二半被"缺一个会触发蒙皮选材的场景"阻塞。**

**未部署、未提交、未晋升稳定。候选 ≠ 发布。**

## 1. 现场与回退（不可混淆）

```
测试环境（可写）        : E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914
玩家安装目录（只读）    : E:\Work\Warcraft III\d3d9.dll
  当前值                : F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3  (基线)
回退工具                : AutoTest/restore_site_dll.py  (rename-park)
绝对不得触碰            : E:\Work\War3   <-- 与玩家安装目录**不是**同一目录

候选 DLL : build32\src\d3d9\d3d9.dll
  字节数  : 36,289,280
  SHA-256 : 77CAEED95684BB596B0E8AACBAEB1B14DC083CD603B09185CC1749546987394D
增量构建  : ninja -C build32 -n  =>  no work to do
```

## 2. 检查点 c2（目标① 的交付物）

```
归档    : E:\Work\WarVK-checkpoint-c2-20260918.zip   26.62 MB
SHA-256 : 0DDE7D4E229B0298663CA5DAF2BFF33EC966A03958ED6A61FEBE8C26856B7992

已验可还原（逐字节）:
  src                : 964 文件  D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5
  AutoTest+shaders+根 : 700 文件  FCC6B3425EA69644307F3A77D423B132BEFA69613C3B09D5565D636A0CD74D01

含    : src, shaders, include, smaa, AutoTest, tools, WarVK
      + meson.build, meson_options.txt, build32_safe.cmd, build-win32.txt, build-win64.txt, AGENTS.md
      + build-options/intro-buildoptions.json
必排除: AutoTest/artifacts/（31,974 MB 运行产物）、build32/
不含  : subprojects/（外部依赖，按 meson wrap 复原）
```

⚠️ **c2 相对当前工作树已过期**（当前 `src` 指纹 = `C2675AFB5FDA39BD7FE6A2D60EEE10383B72CF996F125B38830C65C7F993F4D9`）。
c2 的用途是"回退到创建它那一刻"，**不是"当前候选"的检查点**。

指纹口径（**必须与指纹一起记录**，这是 c1 的教训）见 `2026-09-18-checkpoint-c2-restorability-verified.md` §4。

## 3. 目标② — 三项缺陷（已修，2026-09-18 复核在位）

| 项 | 证据 |
| --- | --- |
| (a) 冷缓存 groupCount | `war3_live_palette_selection.cpp` 共享谓词 `ProducerGroupCountCovers` 调用点 **2**（`:339`/`:391`）；字面比较 **0** |
| (b) nativeKnown | `d3d9_device.cpp:23520` `0u, false)` + R2 注释（`:23517-23519`） |
| (c) 计数器 | 原子变量 `sink.cpp:38/39`；位置 `23505` 子门内 `23506`、session 门内 `23523` |

## 4. 目标③ — 有版本「正常观察链」

### 已完成

| 项 | 说明 |
| --- | --- |
| P0 stage=5 版本门控 | `analyze_palette_object_evidence.py` 新增 `LEGACY_STAGES`+按版本选表 ⇒ v1/v2 遇 stage=5 **整份拒绝**；有**行为**测试（v∈{None,1,2} 拒 + v=3 受） |
| P1 deltaFrames | 读方分支漏了 `FirstSight`；已修 + 多帧夹具 |
| P1 完成判据按链型 | `required_stage_count = 3 if i_firstsight else 4` ⇒ **正常链首次被正面认定** |
| 写方修正① | 首见去重由"按帧"改"按条目"（`war3_palette_object_evidence.h`），并删死字段 `lastFirstSightFrame` |
| 回归锁 | `war3_palette_object_evidence_test.cpp` Case8 末尾多帧"只发一次"用例 |
| **实机 A/B** | 链首覆盖 **32 → 64**（翻倍）、重复发射 **1304 → 64**（↓95%）；站点已恢复 |
| 守卫用例 | `test_event_before_the_chain_head_must_block_completion` 锁"链首前有事件⇒不得认证" |
| G 门禁 | `check_header_block` 参数化（v3=3）⇒ CHECKS 1117→1137/1141，G 的 header 真被断言 |

### ③ 未完成项

| 项 | 阻塞/说明 |
| --- | --- |
| 写方修正② 把 `CanEmit` 预检提到 `Insert` 之前 | 需定"预算紧张时对象完全不可见"的取舍 + **显式丢失计数** |
| 写方修正③ 终态不得给从未发首见的条目兜底写 `Drawn` | **会碰 v1/v2 旧合同**，需谨慎 |
| `FirstSight` 正名 | 真实语义 = "首次进入**该生产路径**"（非"对象首次观察"）；改名需协调 write/两读方/测试/文档且 wire 值不变 |
| 配额论证 | `emitted − terminal = 3584 = kNormalBudget` 仍精确打满；仍有 **43/107** 对象拿不到链首。提高预算**须单独论证**（改变取证开销） |
| G 门禁三函数参数化 | `check_decoded_events`/`check_gaps`/`check_chain` 整段按 4 阶段/5 事件/Recovered 写；套用到 G 会产生 **6 处失败**（已逐条记在源码注释里） |
| P2 三项 | 夹具改真生产形状；新计数器加**行为**校验；场景 G 补齐被跳过的检查 |

## 5. 目标④ — 严格契约与蒙皮对照

### 第一半 ✅

当前构建 `warvk_skin_palette_contract_candidate = True`（`build-options/intro-buildoptions.json`）；
`ContractEnabled()` 在 **19 处**被调用（含真实选择入口 `war3_live_palette_selection.cpp` 与 `war3_model_hook.cpp`(7)）⇒ **非死代码**。

### 第二半：分两层

| 层 | 状态 |
| --- | --- |
| **契约逻辑层** | ✅ `war3_skin_palette_admission_test.exe` **6 用例 / 33 checks / 0 失败**，覆盖四子问题（含 ②(a) 的 groupCount 规则） |
| **实机对照层** | ⏳ **被阻塞**：两次 A/B（`DXVK_WAR3_SKIN_PALETTE_CONTRACT=1` vs `=0`，各 120 s）**分布完全一致**，都 100% 未设 ⇒ **本场景无检验力** |

### 已查清的机制（这是 ④ 的主要收获）

```
产出链:
  d3d9_device.cpp:20321/20452  产 selectedPalette
        :20865-20866          liveRuntimeGroupPaletteReady 为假 ⇒ 回退 packet.paletteSelection
        :20867                attemptedPaletteSelection（记录"尝试值"）
        :20868-20870          ★ if (skinned && ContractEnabled() && !IsSkinPaletteSelectionCurrent)
                                 selectedPalette = {}      <- "提交时被换掉"
        :21533                draw.inputSkinSelection = selectedPalette
  d3d9_war3_scene.h:412        Selection inputSkinSelection
  war3_frame_inputs.cpp:234    r.skinSelection = d.inputSkinSelection
                    :315-324    写成 JSON "skinPaletteSelection"

注意: inputSkinSelection **只在 InputsEnabled() 下赋值**（d3d9_war3_scene.h:413-415 注释）
      ⇒ 看该字段**必须**开 DXVK_WAR3_FRAME_EVIDENCE_INPUTS=1。
      ⇒ 它写在 **.inputs.json** 的 batches[].draws[].skinPaletteSelection，
        **不是**带标签的事件（我先在 label 里找了，白找一轮）。
```

**决定性排除**：`CONTRACT=0` 时 `:20868` 短路 ⇒ **绝不**执行清空 ⇒ 结果仍全空 ⇒
**空样本之因在 `:20870` 之上游**（该推理不依赖观察精度）。

### ④ 下一步（唯一未做的动作）

```
① 判别：本场景**有没有 skinned draw**？
   数据已在手：cpu-5204-65829163662-1.json.inputs.json（run B，CONTRACT=0）
   若**无** skinned draw ⇒ 结论"场景不含蒙皮对象"，④ 第二半需换场景而非改代码；
   若**有** skinned 却仍全空 ⇒ 指向 selectedPalette 上游（rebuildSelection / packet.paletteSelection）。
② 若需换场景：找一个确定含蒙皮单位的地图，重跑 A/B。
③ 记录时必须写明：隔离桌面的**功能对照**，**不是前台性能数据**。
```

## 6. 复现入口（命令）

```powershell
# 构建
build32_safe.cmd src/d3d9/d3d9.dll -j4
ninja -C build32 -n                     # 必须 no work to do

# 静态（全量）
Get-ChildItem AutoTest -File -Filter "test_*_static.py" | ForEach-Object { py $_.FullName }

# meson
ninja -C build32 test

# wire roundtrip 驱动（需 exe 路径参数）
$exe=(Get-ChildItem build32 -Recurse -Filter "*wire*roundtrip*.exe").FullName
py AutoTest/test_palette_object_wire_roundtrip.py $exe      # 期望 CHECKS=1137 FAILURES=0

# 实机采集（自带 备份→部署→运行→取数→恢复；CAND_SHA 必须与当前 DLL 一致）
py AutoTest/live_contrast_palette_objects.py
```

## 7. 最近一次全量门禁结果

```
AutoTest 全量静态 : 259 scripts, 0 failed
meson test       : 85 Ok / 0 Fail
wire roundtrip   : CHECKS=1137 FAILURES=0  CERTIFIED_SPEC_SATISFIED
admission test   : 6 passed, 0 failed (33 checks)
```

**注意**：这些是在**当时那份源码**上取得的。源码自那以后已有改动（见 §4 未完成项），
**不构成"当前状态全门禁通过"**。

## 8. 跨轮教训（避免重复付代价）

| 教训 | 实例 |
| --- | --- |
| 比较失败时先怀疑比较本身 | c2 两次 `MATCH: False` 都是我的脚本错（8.3 短名偏移、单引号不插值） |
| 先落盘再检索 | 过滤流式输出两次没看到 G 的失败行，落盘一次就看到 |
| "没观察到" ≠ "观察到没有" | ④ 的 A/B 全空只是**空样本**，不是阴性证据 |
| 猜测要先去证伪 | 我曾猜"检查计数随并发波动"，三次一致运行证伪 |
| 先确认在看正确的地方 | ⑤ 次：`skin-selection/v1` 不在 label 里；文件名截断显示；误读 `glob[0]`（artifacts 副本） |
| 改任何 `src/` 文件后立刻 `ninja -n` | 一处**注释**改动触发 28 个目标重编译，我漏了 |
| 断言须附实现位置+失败形态+看守测试 | 已发现 **3 处**"文字失真"（见 `2026-09-18-traceable-claims-and-three-text-drifts.md`） |
| 修好后勿顺手放宽相邻限制 | 明确拒绝了放宽 `drawnBeforeEnqueued` / `frameSerial` / 枚举改名三处诱惑 |
---

# 修订（round 249–256）：④ 的链路已完整；③ 的 P2 完成一项

> §1–§8 仍是有效的现场/回退/检查点/复现入口。本节**取代** §4 的"③ 未完成项"与 §5 的"④ 下一步"。

## A. ③ 的变化

| 项 | 变化 |
| --- | --- |
| **P2 新计数器行为校验** | ✅ **已完成**（round 256）：解析器新增两条**单向不变量** —— ①`firstSightEmitted>0 ⇒ firstSightInserted>=1`；②导出中 FirstSight 事件数 `<= firstSightEmitted`。仅 `version>=3` 生效 |
| 该不变量立刻抓到的问题 | **我自己的两个夹具不自洽**（round 221/218 写的：构造了 FirstSight 事件却没声明发射计数）⇒ 已用 `counter_overrides` 修正夹具，**未放宽判据** |
| 仍剩 | 夹具改真生产形状；场景 G 补齐被跳过的三项检查（已有 6 处失败清单，需链型参数化） |
| ③ 其余未完成项 | 不变：写方修正②③、`FirstSight` 正名决策、配额论证（`emitted−terminal=3584` 仍打满，43/107 对象缺链首） |

## B. ④ 的完整状态（这是本轮修订的重点）

### B.1 已完整查清的产出链（全部 `src/` 路径已核实）

```
[最上游] d3d9_device.cpp:20321-20324
  auto selectedPalette = packetAuthoritativeSkinnedContractReady
      ? packet.paletteSelection
      : currentDrawSample != nullptr ? currentDrawSample->paletteSelection
                                     : skin::Selection{};      <- 第三支=全默认/Unknown
        :20452   selectedPalette = rebuildSelection;             （Phase 7.35 路径 2）
        :20865   if (!liveRuntimeGroupPaletteReady)
        :20866     selectedPalette = packet.paletteSelection;    （回退）
        :20867   attemptedPaletteSelection = selectedPalette;
        :20868   if (skinned && ContractEnabled() && !IsSkinPaletteSelectionCurrent(...))
        :20870     selectedPalette = {};                         ★ 提交时换掉
        :21519-23 draw.paletteDiagnostics.{source,slot,captureSerial,publicationTicket,frameTag}
        :21524   if (InputsEnabled()) draw.inputSkinSelection = selectedPalette;

[D 点]  d3d9_war3_shadow.cpp:5249-5251
          MapPaletteObjectSource(uint32_t(diagnostics.source))   <- 显式映射器
        :5245-46 MakePaletteObjectFrames(..., frameTag, frameTag != 0u)  => nativeKnown
        :5252    NoteDrawn(..., frameTag == 0u)                => selectionClearedByNativeOverride

[wire]  palette-object/v1 的 Drawn 事件携带 source 等诊断字段（主导出，不需 INPUTS）
```

### B.2 观察通道（**这里曾经走错**）

| 通道 | 结论 |
| --- | --- |
| **D 点**（`palette-object/v1` 的 `Drawn` 事件） | ✅ **正确通道**。数据在**主导出**里 |
| inputs 导出（`.inputs.json` 的 `draws[].skinPaletteSelection`） | ❌ **结构上看不到选材** —— 它记录 `provenance[0]==2u` 的 draw（`d3d9_device.cpp:23304`），那条路径**从不写 `inputSkinSelection`** |

### B.3 测量结果（本场景）

```
真实 live Drawn（terminal == NoTerminal）: 3072 条
其中 source == Unknown                  : 3072 条 (100%)

A contract=1 与 B contract=0 分布无本质差异（都几乎全 Unknown）
```

### B.4 已排除的解释（5 种，逐个排除）

| 解释 | 排除依据 |
| --- | --- |
| 看错通道 | D 点是正确通道；inputs 导出记录 kind=2，不写该字段 |
| 解析器误读 | 映射规则已查明（`war3_palette_object_capture.h:101-117`）；`skin::Unknown(0)→PaletteObjectSource::Unknown` 与观测一致 |
| `source` 语义混用 | **已证伪**：存在显式 `MapPaletteObjectSource()`（round 252） |
| 契约门清空 | **A/B 逻辑封闭排除**：`CONTRACT=0` 时 `:20868` 短路、绝不清空，而 B 仍全 Unknown |
| `NoSource` 神秘来源 | **已解释**：全部在 `terminal=WindowExpired` 上 ⇒ 是**终态事件**（stage 回填为 `Drawn`、`source` 保持默认 `None`） |

### B.5 剩余二义性（**这是 ④ 唯一的未决点**）

由 `:20321` 三分支可知，全 Unknown 对应两种可能：

| # | 可能 |
| --- | --- |
| 1 | `packetAuthoritativeSkinnedContractReady==false` **且** `currentDrawSample==nullptr` ⇒ 两条上游都没样本 ⇒ **场景未触发该路径**（倾向，未证实） |
| 2 | 样本存在但其 `paletteSelection` 本身即 Unknown ⇒ 指向更上游产出方 |

**分辨途径**：

```
(a) 一个已知会触发蒙皮选材的场景            <- 首选，当前缺
(b) 对 :20321 三分支插桩加诊断计数          <- 可执行，但需"构建+实机采集+恢复"完整周期
```

### B.6 我不能说的（硬约束）

- **不**断定"产出方有缺陷"；
- **不**断定"场景不含蒙皮对象"；
- **不**为了"让 ④ 看起来完成"而填一个解释进去。

**"没有观察到" ≠ "观察到没有"。**

## C. 新增的跨轮教训（补 §8）

| 教训 | 实例 |
| --- | --- |
| **跨文件索引/字段映射必须先看到"写入点与读取点是同一数组"的证据** | round 249：我套用 `inputEvidenceProvenance` 的索引去读 `r.provenance`（两者其实是同一数组，但**初始化模板按 `[0]` 种类不同**）⇒ 差点得出假结论 |
| **"提出怀疑"与"断言缺陷"是两件事** | round 251 标"未判定" → round 252 查证**证伪**（存在映射器），避免了一条假缺陷记录 |
| **假注释不会被任何东西抓到** | 已发现 3 处"文字失真"，其中最严重的一处**主动掩盖了一个覆盖缺口**（"生产身份形状零覆盖"） |
| **"门禁全绿"只在门禁真的会失败时才有意义** | round 256 的新不变量**首次运行就抓到我自己两个不自洽夹具** |

## D. 当前现场（未变）

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
候选 DLL = build32\src\d3d9\d3d9.dll  36,289,280 B  SHA 77CAEED9…
ninja -C build32 -n  =>  no work to do
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```
